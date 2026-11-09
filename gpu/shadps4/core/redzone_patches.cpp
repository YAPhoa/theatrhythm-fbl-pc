// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// theatrhythm: the "Windows static guest red-zone protection" pass of upstream
// src/core/cpu_patches.cpp (PR #4802), without upstream's other CPU patches (the runtime's ISA
// rewrite and TCB load patches cover those) and with a C entry point for the loader (probe.c).
// Windows writes an exception's CONTEXT/XSAVE frame directly below the faulting rsp, so a GPU
// page-tracking fault in a guest leaf function overwrites the 128-byte SysV red zone below it
// (issue #12). Each memory access made while red-zone data is live moves to a trampoline that
// runs it with rsp 128 bytes lower: lea rsp,[rsp-128]; insn; lea rsp,[rsp+128]; jmp back.

#ifdef _WIN32

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <thread>
#include <unordered_set>
#include <vector>
#include <Zydis/Zydis.h>
#include <xbyak/xbyak.h>
#include <xbyak/xbyak_util.h>
#include "common/assert.h"
#include "common/decoder.h"
#include "common/types.h"
#include "../../theatrhythm_gpu.h"

using namespace Xbyak::util;

namespace {

constexpr size_t NearJumpSize = 5;
constexpr size_t GuestRedZoneSize = 128;
constexpr size_t ShortJumpSize = 2;
using RedZoneMask = std::bitset<GuestRedZoneSize>;

struct DecodedCodeInstruction {
    uintptr_t address{};
    ZydisDecodedInstruction instruction{};
    std::array<ZydisDecodedOperand, ZYDIS_MAX_OPERAND_COUNT> operands{};
    RedZoneMask red_zone_use{};
    RedZoneMask red_zone_def{};
    RedZoneMask red_zone_live{};
    bool accesses_memory{};
    bool uses_stack_pointer{};
    bool has_red_zone_operand{};
    bool has_unmodeled_red_zone_operand{};
    bool changes_stack_pointer{};
    bool replaces_stack_pointer{};
    std::optional<s64> stack_pointer_delta;
};

struct DecodedFunction {
    std::map<uintptr_t, DecodedCodeInstruction> instructions;
    std::set<uintptr_t> branch_targets;
    bool uses_red_zone{};
    bool has_indirect_branch{};
    bool requires_conservative_red_zone_tracking{};
    // theatrhythm: each resolved jump table's targets: the jump's successors for liveness.
    std::map<uintptr_t, std::vector<uintptr_t>> table_targets;
};

struct InstructionRewrite {
    bool protect_red_zone{};
};

bool IsStackPointerRegister(ZydisRegister reg) {
    return reg != ZYDIS_REGISTER_NONE &&
           ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg) == ZYDIS_REGISTER_RSP;
}

bool IsControlFlowTerminator(const ZydisDecodedInstruction& instruction) {
    return instruction.meta.category == ZYDIS_CATEGORY_UNCOND_BR ||
           instruction.meta.category == ZYDIS_CATEGORY_RET ||
           instruction.meta.category == ZYDIS_CATEGORY_INTERRUPT ||
           instruction.meta.category == ZYDIS_CATEGORY_SYSRET ||
           instruction.mnemonic == ZYDIS_MNEMONIC_UD2;
}

uintptr_t GetRelativeTarget(const DecodedCodeInstruction& decoded) {
    for (u8 index = 0; index < decoded.instruction.operand_count_visible; ++index) {
        const auto& operand = decoded.operands[index];
        if (operand.type != ZYDIS_OPERAND_TYPE_IMMEDIATE || !operand.imm.is_relative) {
            continue;
        }

        ZyanU64 target{};
        if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&decoded.instruction, &operand, decoded.address,
                                                  &target))) {
            return target;
        }
    }
    return 0;
}

DecodedCodeInstruction DecodeCodeInstruction(uintptr_t address, uintptr_t end) {
    DecodedCodeInstruction decoded{.address = address};
    const auto status = Common::Decoder::Instance()->decodeInstruction(
        decoded.instruction, decoded.operands.data(), reinterpret_cast<void*>(address),
        end - address);
    if (!ZYAN_SUCCESS(status)) {
        decoded.instruction.length = 0;
        return decoded;
    }

    for (u8 index = 0; index < decoded.instruction.operand_count; ++index) {
        const auto& operand = decoded.operands[index];
        if (operand.type == ZYDIS_OPERAND_TYPE_REGISTER) {
            decoded.uses_stack_pointer |= IsStackPointerRegister(operand.reg.value);
            if (IsStackPointerRegister(operand.reg.value) &&
                (operand.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0 &&
                decoded.instruction.meta.category != ZYDIS_CATEGORY_CALL &&
                decoded.instruction.meta.category != ZYDIS_CATEGORY_RET) {
                decoded.changes_stack_pointer = true;
            }
            continue;
        }
        if (operand.type != ZYDIS_OPERAND_TYPE_MEMORY) {
            continue;
        }

        const bool stack_relative =
            IsStackPointerRegister(operand.mem.base) || IsStackPointerRegister(operand.mem.index);
        decoded.uses_stack_pointer |= stack_relative;
        constexpr ZydisOperandActions MemoryAccessMask =
            ZYDIS_OPERAND_ACTION_MASK_READ | ZYDIS_OPERAND_ACTION_MASK_WRITE;
        if (decoded.instruction.mnemonic != ZYDIS_MNEMONIC_LEA &&
            decoded.instruction.mnemonic != ZYDIS_MNEMONIC_NOP && !stack_relative &&
            (operand.actions & MemoryAccessMask) != 0) {
            decoded.accesses_memory = true;
        }
    }

    if (decoded.changes_stack_pointer) {
        const auto& operands = decoded.operands;
        if (decoded.instruction.mnemonic == ZYDIS_MNEMONIC_MOV &&
            operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
            IsStackPointerRegister(operands[0].reg.value) &&
            operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            decoded.replaces_stack_pointer = true;
        } else if ((decoded.instruction.mnemonic == ZYDIS_MNEMONIC_ADD ||
                    decoded.instruction.mnemonic == ZYDIS_MNEMONIC_SUB) &&
                   operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                   IsStackPointerRegister(operands[0].reg.value) &&
                   operands[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            const s64 immediate = operands[1].imm.is_signed
                                      ? operands[1].imm.value.s
                                      : static_cast<s64>(operands[1].imm.value.u);
            decoded.stack_pointer_delta =
                decoded.instruction.mnemonic == ZYDIS_MNEMONIC_ADD ? immediate : -immediate;
        } else if (decoded.instruction.mnemonic == ZYDIS_MNEMONIC_LEA &&
                   operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                   IsStackPointerRegister(operands[0].reg.value) &&
                   operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                   IsStackPointerRegister(operands[1].mem.base) &&
                   operands[1].mem.index == ZYDIS_REGISTER_NONE) {
            decoded.stack_pointer_delta = operands[1].mem.disp.value;
        } else if (decoded.instruction.mnemonic == ZYDIS_MNEMONIC_PUSH ||
                   decoded.instruction.mnemonic == ZYDIS_MNEMONIC_PUSHF ||
                   decoded.instruction.mnemonic == ZYDIS_MNEMONIC_PUSHFD ||
                   decoded.instruction.mnemonic == ZYDIS_MNEMONIC_PUSHFQ) {
            decoded.stack_pointer_delta = -static_cast<s64>(sizeof(u64));
        } else if (decoded.instruction.mnemonic == ZYDIS_MNEMONIC_POP ||
                   decoded.instruction.mnemonic == ZYDIS_MNEMONIC_POPF ||
                   decoded.instruction.mnemonic == ZYDIS_MNEMONIC_POPFD ||
                   decoded.instruction.mnemonic == ZYDIS_MNEMONIC_POPFQ) {
            decoded.stack_pointer_delta = sizeof(u64);
        }
    }

    for (u8 index = 0; index < decoded.instruction.operand_count_visible; ++index) {
        const auto& operand = decoded.operands[index];
        if (operand.type != ZYDIS_OPERAND_TYPE_MEMORY ||
            !IsStackPointerRegister(operand.mem.base) || !operand.mem.disp.has_displacement ||
            operand.mem.disp.value >= 0) {
            continue;
        }

        decoded.has_red_zone_operand = true;
        if (decoded.instruction.mnemonic == ZYDIS_MNEMONIC_LEA) {
            if (decoded.stack_pointer_delta.has_value()) {
                decoded.has_red_zone_operand = false;
                continue;
            }
            decoded.has_unmodeled_red_zone_operand = true;
            continue;
        }

        const s64 access_start = operand.mem.disp.value;
        const s64 access_size = std::max<s64>(operand.size / 8, 1);
        const s64 range_start = std::max(access_start, -static_cast<s64>(GuestRedZoneSize));
        const s64 range_end = std::min(access_start + access_size, 0LL);
        for (s64 offset = range_start; offset < range_end; ++offset) {
            const size_t bit = static_cast<size_t>(offset + static_cast<s64>(GuestRedZoneSize));
            if ((operand.actions & ZYDIS_OPERAND_ACTION_MASK_READ) != 0) {
                decoded.red_zone_use.set(bit);
            }
            if ((operand.actions & ZYDIS_OPERAND_ACTION_WRITE) != 0) {
                decoded.red_zone_def.set(bit);
            }
        }
    }
    return decoded;
}

bool IsSameRegister(ZydisRegister lhs, ZydisRegister rhs) {
    return lhs != ZYDIS_REGISTER_NONE && rhs != ZYDIS_REGISTER_NONE &&
           ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, lhs) ==
               ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, rhs);
}

bool WritesRegister(const DecodedCodeInstruction& decoded, ZydisRegister reg) {
    return std::ranges::any_of(std::span{decoded.operands}.first(decoded.instruction.operand_count),
                               [reg](const ZydisDecodedOperand& operand) {
                                   return operand.type == ZYDIS_OPERAND_TYPE_REGISTER &&
                                          (operand.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) !=
                                              0 &&
                                          IsSameRegister(operand.reg.value, reg);
                               });
}

std::optional<std::vector<uintptr_t>> ResolveBoundedJumpTable(
    const DecodedFunction& function, uintptr_t branch_address, uintptr_t function_start,
    uintptr_t function_end, uintptr_t segment_start, uintptr_t segment_end) {
    const auto branch = function.instructions.find(branch_address);
    if (branch == function.instructions.end() ||
        branch->second.instruction.mnemonic != ZYDIS_MNEMONIC_JMP ||
        branch->second.operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER) {
        return std::nullopt;
    }
    const ZydisRegister target_reg = branch->second.operands[0].reg.value;

    const auto previous_contiguous = [&function](auto instruction) {
        if (instruction == function.instructions.begin()) {
            return function.instructions.end();
        }
        const auto previous = std::prev(instruction);
        return previous->first + previous->second.instruction.length == instruction->first
                   ? previous
                   : function.instructions.end();
    };

    constexpr size_t MaxInterveningInstructions = 4;
    auto add = function.instructions.end();
    auto pattern_cursor = branch;
    for (size_t count = 0; count <= MaxInterveningInstructions; ++count) {
        const auto candidate = previous_contiguous(pattern_cursor);
        if (candidate == function.instructions.end()) {
            break;
        }
        const auto& decoded = candidate->second;
        if (decoded.instruction.mnemonic == ZYDIS_MNEMONIC_ADD &&
            decoded.operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
            IsSameRegister(decoded.operands[0].reg.value, target_reg) &&
            decoded.operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
            add = candidate;
            break;
        }
        if (WritesRegister(decoded, target_reg) || IsControlFlowTerminator(decoded.instruction)) {
            return std::nullopt;
        }
        pattern_cursor = candidate;
    }
    if (add == function.instructions.end()) {
        return std::nullopt;
    }
    const ZydisRegister table_reg = add->second.operands[1].reg.value;

    // theatrhythm: compilers schedule unrelated instructions between the table load and the add
    // (eboot 0xd34845: movsxd rcx,[r10+rbp*4]; mov rbx,[rax+rbx+0x18]; add rcx,r10; jmp rcx).
    // Skip a few that leave the target and table registers alone; upstream wants the two adjacent,
    // and a table it cannot resolve hides every block reached only through it from the pass.
    const auto is_table_load = [&](const DecodedCodeInstruction& decoded) {
        return decoded.instruction.mnemonic == ZYDIS_MNEMONIC_MOVSXD &&
               decoded.operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
               IsSameRegister(decoded.operands[0].reg.value, target_reg);
    };
    auto load = previous_contiguous(add);
    for (size_t count = 0; count < MaxInterveningInstructions &&
                           load != function.instructions.end() && !is_table_load(load->second);
         ++count) {
        const auto& decoded = load->second;
        if (WritesRegister(decoded, target_reg) || WritesRegister(decoded, table_reg) ||
            IsControlFlowTerminator(decoded.instruction) ||
            decoded.instruction.meta.category == ZYDIS_CATEGORY_COND_BR ||
            decoded.instruction.meta.category == ZYDIS_CATEGORY_CALL) {
            return std::nullopt;
        }
        load = previous_contiguous(load);
    }
    if (load == function.instructions.end() ||
        load->second.instruction.mnemonic != ZYDIS_MNEMONIC_MOVSXD ||
        load->second.operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER ||
        !IsSameRegister(load->second.operands[0].reg.value, target_reg) ||
        load->second.operands[1].type != ZYDIS_OPERAND_TYPE_MEMORY ||
        !IsSameRegister(load->second.operands[1].mem.base, table_reg) ||
        load->second.operands[1].mem.index == ZYDIS_REGISTER_NONE ||
        load->second.operands[1].mem.scale != sizeof(s32)) {
        return std::nullopt;
    }
    const ZydisRegister index_reg = load->second.operands[1].mem.index;

    constexpr size_t MaxPatternInstructions = 64;
    std::optional<size_t> table_size;
    std::optional<uintptr_t> guarded_path_start;
    auto cursor = load;
    for (size_t count = 0; count < MaxPatternInstructions; ++count) {
        cursor = previous_contiguous(cursor);
        if (cursor == function.instructions.end()) {
            break;
        }
        const auto& decoded = cursor->second;
        if (decoded.instruction.mnemonic == ZYDIS_MNEMONIC_CMP &&
            decoded.operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
            IsSameRegister(decoded.operands[0].reg.value, index_reg) &&
            decoded.operands[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            const uintptr_t next_address = decoded.address + decoded.instruction.length;
            const auto bounds_branch = function.instructions.find(next_address);
            if (bounds_branch == function.instructions.end()) {
                return std::nullopt;
            }
            const s64 bound = decoded.operands[1].imm.is_signed
                                  ? decoded.operands[1].imm.value.s
                                  : static_cast<s64>(decoded.operands[1].imm.value.u);
            if (bound < 0) {
                return std::nullopt;
            }
            if (bounds_branch->second.instruction.mnemonic == ZYDIS_MNEMONIC_JNBE) {
                table_size = static_cast<size_t>(bound) + 1;
            } else if (bounds_branch->second.instruction.mnemonic == ZYDIS_MNEMONIC_JNB) {
                table_size = static_cast<size_t>(bound);
            } else {
                return std::nullopt;
            }
            guarded_path_start = next_address + bounds_branch->second.instruction.length;
            break;
        }
        if (WritesRegister(decoded, index_reg)) {
            return std::nullopt;
        }
    }
    if (!table_size) {
        return std::nullopt;
    }
    if (std::ranges::any_of(function.branch_targets, [&](uintptr_t target) {
            return target >= *guarded_path_start && target <= branch_address;
        })) {
        return std::nullopt;
    }

    const auto decode_table_address =
        [table_reg](const DecodedCodeInstruction& decoded) -> std::optional<uintptr_t> {
        if (decoded.instruction.mnemonic != ZYDIS_MNEMONIC_LEA ||
            decoded.operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER ||
            !IsSameRegister(decoded.operands[0].reg.value, table_reg) ||
            decoded.operands[1].type != ZYDIS_OPERAND_TYPE_MEMORY) {
            return std::nullopt;
        }
        ZyanU64 absolute_address{};
        if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&decoded.instruction, &decoded.operands[1],
                                                   decoded.address, &absolute_address))) {
            return std::nullopt;
        }
        return absolute_address;
    };

    std::set<uintptr_t> table_candidates;
    cursor = load;
    for (size_t count = 0; count < MaxPatternInstructions; ++count) {
        cursor = previous_contiguous(cursor);
        if (cursor == function.instructions.end()) {
            break;
        }
        if (!WritesRegister(cursor->second, table_reg)) {
            continue;
        }
        if (const auto address = decode_table_address(cursor->second)) {
            table_candidates.insert(*address);
        }
        break;
    }
    if (table_candidates.empty()) {
        for (const auto& [address, decoded] : function.instructions) {
            if (address >= branch_address) {
                break;
            }
            if (const auto table_address = decode_table_address(decoded)) {
                table_candidates.insert(*table_address);
            }
        }
    }
    constexpr size_t MaxJumpTableEntries = 4096;
    if (*table_size == 0 || *table_size > MaxJumpTableEntries) {
        return std::nullopt;
    }

    std::optional<std::vector<uintptr_t>> resolved_targets;
    for (const uintptr_t table_address : table_candidates) {
        if (table_address < segment_start || table_address > segment_end ||
            *table_size > (segment_end - table_address) / sizeof(s32)) {
            continue;
        }

        std::vector<uintptr_t> targets;
        targets.reserve(*table_size);
        bool valid = true;
        for (size_t index = 0; index < *table_size; ++index) {
            s32 offset;
            std::memcpy(&offset,
                        reinterpret_cast<const void*>(table_address + index * sizeof(offset)),
                        sizeof(offset));
            const s64 target = static_cast<s64>(table_address) + offset;
            if (target < static_cast<s64>(function_start) ||
                target >= static_cast<s64>(function_end)) {
                valid = false;
                break;
            }
            targets.push_back(static_cast<uintptr_t>(target));
        }
        if (!valid) {
            continue;
        }
        std::ranges::sort(targets);
        targets.erase(std::ranges::unique(targets).begin(), targets.end());
        if (resolved_targets) {
            return std::nullopt;
        }
        resolved_targets = std::move(targets);
    }
    return resolved_targets;
}

DecodedFunction DecodeFunction(uintptr_t function_start, uintptr_t function_end,
                               uintptr_t segment_start, uintptr_t segment_end) {
    DecodedFunction function;
    std::vector<uintptr_t> blocks{function_start};
    std::unordered_set<uintptr_t> visited;
    std::set<uintptr_t> indirect_branches;
    std::set<uintptr_t> resolved_indirect_branches;

    while (true) {
        while (!blocks.empty()) {
            uintptr_t address = blocks.back();
            blocks.pop_back();

            while (address >= function_start && address < function_end &&
                   !visited.contains(address)) {
                visited.insert(address);
                auto decoded = DecodeCodeInstruction(address, function_end);
                if (decoded.instruction.length == 0) {
                    break;
                }

                function.uses_red_zone |= decoded.has_red_zone_operand;
                function.requires_conservative_red_zone_tracking |=
                    decoded.has_unmodeled_red_zone_operand ||
                    (decoded.changes_stack_pointer && !decoded.stack_pointer_delta.has_value());

                const uintptr_t next_address = address + decoded.instruction.length;
                const uintptr_t branch_target = GetRelativeTarget(decoded);
                if (branch_target >= function_start && branch_target < function_end) {
                    function.branch_targets.insert(branch_target);
                }
                function.instructions.emplace(address, decoded);

                if (decoded.instruction.meta.category == ZYDIS_CATEGORY_COND_BR) {
                    if (branch_target >= function_start && branch_target < function_end) {
                        blocks.push_back(branch_target);
                    }
                } else if (IsControlFlowTerminator(decoded.instruction)) {
                    if (decoded.instruction.meta.category == ZYDIS_CATEGORY_UNCOND_BR &&
                        branch_target >= function_start && branch_target < function_end) {
                        blocks.push_back(branch_target);
                    } else if (decoded.instruction.meta.category == ZYDIS_CATEGORY_UNCOND_BR &&
                               branch_target == 0) {
                        indirect_branches.insert(address);
                    }
                    break;
                }
                address = next_address;
            }
        }

        bool discovered_block = false;
        for (const uintptr_t branch_address : indirect_branches) {
            if (resolved_indirect_branches.contains(branch_address)) {
                continue;
            }
            const auto targets = ResolveBoundedJumpTable(function, branch_address, function_start,
                                                         function_end, segment_start, segment_end);
            if (!targets) {
                continue;
            }
            resolved_indirect_branches.insert(branch_address);
            function.table_targets[branch_address] = *targets;
            for (const uintptr_t target : *targets) {
                function.branch_targets.insert(target);
                if (!visited.contains(target)) {
                    blocks.push_back(target);
                    discovered_block = true;
                }
            }
        }
        if (!discovered_block) {
            break;
        }
    }
    function.has_indirect_branch = indirect_branches.size() != resolved_indirect_branches.size();
    return function;
}

RedZoneMask TranslateRedZoneMask(const RedZoneMask& mask, s64 stack_pointer_delta) {
    RedZoneMask translated;
    for (size_t bit = 0; bit < GuestRedZoneSize; ++bit) {
        if (!mask.test(bit)) {
            continue;
        }
        const s64 after_offset = static_cast<s64>(bit) - static_cast<s64>(GuestRedZoneSize);
        const s64 before_offset = after_offset + stack_pointer_delta;
        if (before_offset >= -static_cast<s64>(GuestRedZoneSize) && before_offset < 0) {
            translated.set(static_cast<size_t>(before_offset + static_cast<s64>(GuestRedZoneSize)));
        }
    }
    return translated;
}

void AnalyzeRedZoneLiveness(DecodedFunction& function) {
    if (!function.uses_red_zone) {
        return;
    }

    if (function.has_indirect_branch || function.requires_conservative_red_zone_tracking ||
        std::ranges::any_of(function.branch_targets, [&function](uintptr_t target) {
            return !function.instructions.contains(target);
        })) {
        for (auto& [_, decoded] : function.instructions) {
            decoded.red_zone_live.set();
        }
        return;
    }

    std::vector<DecodedCodeInstruction*> instructions;
    instructions.reserve(function.instructions.size());
    std::map<uintptr_t, size_t> indices;
    for (auto& [address, decoded] : function.instructions) {
        indices.emplace(address, instructions.size());
        instructions.push_back(&decoded);
    }

    std::vector<RedZoneMask> live_in(instructions.size());
    bool changed;
    do {
        changed = false;
        for (size_t reverse_index = instructions.size(); reverse_index-- > 0;) {
            const auto& decoded = *instructions[reverse_index];
            RedZoneMask live_out;

            const auto add_successor = [&](uintptr_t address) {
                if (const auto successor = indices.find(address); successor != indices.end()) {
                    live_out |= live_in[successor->second];
                }
            };

            const uintptr_t next_address = decoded.address + decoded.instruction.length;
            const uintptr_t branch_target = GetRelativeTarget(decoded);
            if (decoded.instruction.meta.category == ZYDIS_CATEGORY_COND_BR) {
                add_successor(next_address);
                add_successor(branch_target);
            } else if (decoded.instruction.meta.category == ZYDIS_CATEGORY_UNCOND_BR) {
                add_successor(branch_target);
                // theatrhythm: upstream gives a jump table's `jmp reg` no successors, so data
                // read again in its cases (eboot 0xd34790's [rsp-0x10], reloaded in each case of
                // its loop) counted as dead before the dispatch and after the last use in a case.
                if (const auto table = function.table_targets.find(decoded.address);
                    table != function.table_targets.end()) {
                    for (const uintptr_t target : table->second) {
                        add_successor(target);
                    }
                }
            } else if (!IsControlFlowTerminator(decoded.instruction)) {
                add_successor(next_address);
            }

            const RedZoneMask translated_live_out =
                decoded.stack_pointer_delta.has_value()
                    ? TranslateRedZoneMask(live_out, *decoded.stack_pointer_delta)
                    : live_out;
            const RedZoneMask new_live_in =
                decoded.red_zone_use | (translated_live_out & ~decoded.red_zone_def);
            if (new_live_in != live_in[reverse_index]) {
                live_in[reverse_index] = new_live_in;
                changed = true;
            }
        }
    } while (changed);

    for (size_t index = 0; index < instructions.size(); ++index) {
        instructions[index]->red_zone_live = live_in[index];
    }
}

bool EncodeRelocatedInstruction(const DecodedCodeInstruction& decoded,
                                Xbyak::CodeGenerator& generator) {
    ZydisEncoderRequest request;
    if (!ZYAN_SUCCESS(ZydisEncoderDecodedInstructionToEncoderRequest(
            &decoded.instruction, decoded.operands.data(),
            decoded.instruction.operand_count_visible, &request))) {
        return false;
    }

    for (u8 index = 0; index < decoded.instruction.operand_count_visible; ++index) {
        const auto& operand = decoded.operands[index];
        if (operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && operand.imm.is_relative) {
            ZyanU64 absolute_address{};
            if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&decoded.instruction, &operand,
                                                       decoded.address, &absolute_address))) {
                return false;
            }
            request.operands[index].imm.u = absolute_address;
        } else if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY &&
                   (operand.mem.base == ZYDIS_REGISTER_RIP ||
                    operand.mem.base == ZYDIS_REGISTER_EIP)) {
            ZyanU64 absolute_address{};
            if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&decoded.instruction, &operand,
                                                       decoded.address, &absolute_address))) {
                return false;
            }
            request.operands[index].mem.displacement = static_cast<ZyanI64>(absolute_address);
        }
    }

    std::array<u8, ZYDIS_MAX_INSTRUCTION_LENGTH> encoded{};
    ZyanUSize encoded_size = encoded.size();
    if (!ZYAN_SUCCESS(ZydisEncoderEncodeInstructionAbsolute(
            &request, encoded.data(), &encoded_size,
            reinterpret_cast<ZyanU64>(generator.getCurr())))) {
        return false;
    }
    generator.db(encoded.data(), encoded_size);
    return true;
}

// theatrhythm: a byte scan before decoding. A red-zone operand, [rsp - n], is ModRM mod 01/10 with
// rm 100, then a SIB byte with base 100 and a negative disp8/disp32. Functions without that byte
// pattern (most of them) are not decoded; false positives are decoded as before.
bool MayUseRedZone(const u8* start, const u8* end) {
    for (const u8* p = start; p + 3 <= end; ++p) {
        if ((p[1] & 7) != 4) {
            continue;
        }
        if (((p[0] & 0xc7) == 0x44 && p[2] >= 0x80) ||
            ((p[0] & 0xc7) == 0x84 && p + 6 <= end && p[5] >= 0x80)) {
            return true;
        }
    }
    return false;
}

// theatrhythm: upstream's PatchModule, one for the whole linked image.
struct PatchModule {
    uintptr_t image;
    bool verbose;
    std::set<u8*> patched;
    Xbyak::CodeGenerator patch_gen;
    Xbyak::CodeGenerator trampoline_gen;
    bool trampoline_exhausted{};
    PatchModule(u8* image_, u64 image_size, u8* trampolines, u64 trampolines_size, bool verbose_)
        : image(reinterpret_cast<uintptr_t>(image_)), verbose(verbose_), patch_gen(image_size, image_),
          trampoline_gen(trampolines_size, trampolines) {}
};

bool HandleTrampolineError(PatchModule* module, const Xbyak::Error& error) {
    if (static_cast<int>(error) != Xbyak::ERR_CODE_IS_TOO_BIG) {
        return false;
    }
    if (!module->trampoline_exhausted) {
        std::fprintf(stderr, "Red zone: trampoline space exhausted\n");
        module->trampoline_exhausted = true;
    }
    return true;
}

// theatrhythm: `skip_starts` holds functions the loader's own patches changed (hooks, the ISA
// rewrite): their stubs jump back into them, so none of their instructions may move.
void PatchRedZoneMemoryInstructions(PatchModule* module, u64 segment_addr, u64 segment_size,
                                    std::span<const uintptr_t> function_starts,
                                    const std::set<uintptr_t>& skip_starts,
                                    TheatrhythmRedZoneResult& result) {

    const uintptr_t segment_end = segment_addr + segment_size;
    std::vector<uintptr_t> starts;
    starts.reserve(function_starts.size());
    for (const uintptr_t start : function_starts) {
        if (start >= segment_addr && start < segment_end) {
            starts.push_back(start);
        }
    }
    std::ranges::sort(starts);
    const auto unique_end = std::ranges::unique(starts).begin();
    starts.erase(unique_end, starts.end());

    // theatrhythm: functions are decoded and analysed on every core first. Each reads only its own
    // bytes, and the patching below writes only inside the function it patches, so the result is
    // the same as upstream's one pass.
    std::vector<std::optional<DecodedFunction>> analyzed(starts.size());
    std::atomic<size_t> next_function{0};
    std::atomic<u64> decoded_instructions{0};
    const auto analyze = [&] {
        for (size_t index; (index = next_function.fetch_add(1)) < starts.size();) {
            const uintptr_t start = starts[index];
            const uintptr_t end = index + 1 < starts.size() ? starts[index + 1] : segment_end;
            if (end <= start || skip_starts.contains(start) ||
                !MayUseRedZone(reinterpret_cast<const u8*>(start), reinterpret_cast<const u8*>(end))) {
                continue;
            }
            auto function = DecodeFunction(start, end, segment_addr, segment_end);
            AnalyzeRedZoneLiveness(function);
            decoded_instructions += function.instructions.size();
            if (function.uses_red_zone) {
                analyzed[index] = std::move(function);
            }
        }
    };
    {
        std::vector<std::thread> workers(std::max(1u, std::thread::hardware_concurrency()));
        for (auto& worker : workers) {
            worker = std::thread{analyze};
        }
        for (auto& worker : workers) {
            worker.join();
        }
    }
    result.instruction_count += decoded_instructions;

    for (size_t function_index = 0; function_index < starts.size(); ++function_index) {
        const uintptr_t function_start = starts[function_index];
        const uintptr_t function_end =
            function_index + 1 < starts.size() ? starts[function_index + 1] : segment_end;
        if (function_end <= function_start) {
            continue;
        }

        ++result.function_count;
        if (skip_starts.contains(function_start)) {
            ++result.skipped_function_count;
            continue;
        }
        if (!analyzed[function_index]) {
            continue;
        }
        auto& function = *analyzed[function_index];

        std::map<uintptr_t, InstructionRewrite> rewrite_sites;

        if (function.uses_red_zone) {
            ++result.red_zone_function_count;
            result.indirect_red_zone_function_count += function.has_indirect_branch;
            for (const auto& [address, decoded] : function.instructions) {
                if (!decoded.accesses_memory || !decoded.red_zone_live.any()) {
                    continue;
                }
                ++result.memory_instruction_count;
                if (decoded.instruction.length < NearJumpSize) {
                    ++result.short_memory_instruction_count;
                }
                // theatrhythm: upstream also moves `call [mem]` to a trampoline. Its return
                // address would then have no unwind data (libc's unwinder, IL2CPP's C++ exceptions
                // and stack traces walk through it), and red-zone data cannot be live across a
                // call, which itself writes below rsp: such calls count as control flow below.
                if (decoded.uses_stack_pointer && !decoded.replaces_stack_pointer) {
                    ++result.stack_dependent_memory_instruction_count;
                    continue;
                }
                if (decoded.instruction.meta.category == ZYDIS_CATEGORY_CALL ||
                    IsControlFlowTerminator(decoded.instruction)) {
                    ++result.control_flow_memory_instruction_count;
                    continue;
                }
                rewrite_sites[address].protect_red_zone = true;
            }
        }
        if (rewrite_sites.empty()) {
            continue;
        }

        struct RelocationSpan {
            std::vector<const DecodedCodeInstruction*> instructions;
            uintptr_t patch_start{};
            uintptr_t continuation{};
            size_t patch_size{};
        };

        const auto emit_span = [&](const RelocationSpan& span) -> std::optional<size_t> {
            const size_t trampoline_offset = module->trampoline_gen.getSize();
            if (module->trampoline_exhausted) {
                return std::nullopt;
            }
            try {
                for (const auto* decoded : span.instructions) {
                    const auto rewrite = rewrite_sites.find(decoded->address);
                    const bool protect_red_zone =
                        rewrite != rewrite_sites.end() && rewrite->second.protect_red_zone;
                    if (protect_red_zone) {
                        module->trampoline_gen.lea(rsp, ptr[rsp - GuestRedZoneSize]);
                    }
                    if (!EncodeRelocatedInstruction(*decoded, module->trampoline_gen)) {
                        module->trampoline_gen.setSize(trampoline_offset);
                        return std::nullopt;
                    }
                    if (protect_red_zone && !decoded->replaces_stack_pointer) {
                        module->trampoline_gen.lea(rsp, ptr[rsp + GuestRedZoneSize]);
                    }
                }
                module->trampoline_gen.jmp(reinterpret_cast<void*>(span.continuation));
            } catch (const Xbyak::Error& error) {
                module->trampoline_gen.setSize(trampoline_offset);
                if (HandleTrampolineError(module, error)) {
                    return std::nullopt;
                }
                throw;
            }
            return trampoline_offset;
        };

        const auto record_rewrites = [&](const RelocationSpan& span) {
            for (const auto* decoded : span.instructions) {
                const auto rewrite = rewrite_sites.find(decoded->address);
                if (rewrite == rewrite_sites.end()) {
                    continue;
                }
                if (rewrite->second.protect_red_zone) {
                    ++result.patched_memory_instruction_count;
                }
                module->patched.insert(reinterpret_cast<u8*>(decoded->address));
            }
        };

        std::vector<std::pair<uintptr_t, uintptr_t>> patched_spans;
        std::vector<uintptr_t> relay_slots;
        std::vector<uintptr_t> short_relay_slots;
        std::vector<uintptr_t> unresolved_sites;
        uintptr_t covered_until{};
        for (auto site_it = rewrite_sites.begin(); site_it != rewrite_sites.end(); ++site_it) {
            const uintptr_t site = site_it->first;
            if (site < covered_until) {
                continue;
            }

            const auto collect_forward_span = [&]() -> std::optional<RelocationSpan> {
                RelocationSpan span{.patch_start = site, .continuation = site};
                while (span.patch_size < NearJumpSize) {
                    const auto decoded_it = function.instructions.find(span.continuation);
                    if (decoded_it == function.instructions.end() ||
                        (span.continuation != site &&
                         function.branch_targets.contains(span.continuation))) {
                        return std::nullopt;
                    }

                    const auto& decoded = decoded_it->second;
                    if (decoded.instruction.meta.category == ZYDIS_CATEGORY_CALL) {
                        return std::nullopt; // theatrhythm: see above
                    }
                    span.instructions.push_back(&decoded);
                    span.patch_size += decoded.instruction.length;
                    span.continuation += decoded.instruction.length;
                    if (span.patch_size < NearJumpSize &&
                        IsControlFlowTerminator(decoded.instruction)) {
                        return std::nullopt;
                    }
                }
                return span;
            };

            const auto collect_backward_span = [&]() -> std::optional<RelocationSpan> {
                const auto site_instruction = function.instructions.find(site);
                ASSERT(site_instruction != function.instructions.end());
                RelocationSpan span{
                    .instructions = {&site_instruction->second},
                    .patch_start = site,
                    .continuation = site + site_instruction->second.instruction.length,
                    .patch_size = site_instruction->second.instruction.length,
                };

                while (span.patch_size < NearJumpSize) {
                    const auto previous_end = function.instructions.lower_bound(span.patch_start);
                    if (previous_end == function.instructions.begin() ||
                        function.branch_targets.contains(span.patch_start)) {
                        return std::nullopt;
                    }

                    const auto previous = std::prev(previous_end);
                    const auto& decoded = previous->second;
                    if (previous->first + decoded.instruction.length != span.patch_start ||
                        previous->first < covered_until ||
                        IsControlFlowTerminator(decoded.instruction) ||
                        decoded.instruction.meta.category == ZYDIS_CATEGORY_CALL) {
                        return std::nullopt;
                    }

                    span.instructions.insert(span.instructions.begin(), &decoded);
                    span.patch_start = previous->first;
                    span.patch_size += decoded.instruction.length;
                }
                return span;
            };

            std::optional<RelocationSpan> selected_span;
            std::optional<size_t> trampoline_offset;
            const auto& site_instruction = function.instructions.at(site);
            const bool can_relocate_neighbors = !function.has_indirect_branch;
            if (can_relocate_neighbors || site_instruction.instruction.length >= NearJumpSize) {
                auto forward_span = collect_forward_span();
                if (forward_span) {
                    trampoline_offset = emit_span(*forward_span);
                    if (trampoline_offset) {
                        selected_span = std::move(forward_span);
                    }
                }
            }
            if (!selected_span && can_relocate_neighbors) {
                if (auto backward_span = collect_backward_span()) {
                    trampoline_offset = emit_span(*backward_span);
                    if (trampoline_offset) {
                        selected_span = std::move(backward_span);
                    }
                }
            }
            if (!selected_span) {
                unresolved_sites.push_back(site);
                continue;
            }

            const auto& span = *selected_span;
            const auto* trampoline = module->trampoline_gen.getCode() + *trampoline_offset;

            auto& patch_gen = module->patch_gen;
            patch_gen.reset();
            patch_gen.setSize(span.patch_start - reinterpret_cast<uintptr_t>(patch_gen.getCode()));
            patch_gen.jmp(trampoline, Xbyak::CodeGenerator::LabelType::T_NEAR);
            patch_gen.nop(span.patch_size - NearJumpSize);

            record_rewrites(span);
            patched_spans.emplace_back(span.patch_start, span.continuation);
            if (span.patch_size >= NearJumpSize * 2) {
                relay_slots.push_back(span.patch_start + NearJumpSize);
            }
            if (span.patch_size >= NearJumpSize + ShortJumpSize) {
                short_relay_slots.push_back(span.patch_start + NearJumpSize);
            }
            covered_until = span.continuation;
        }

        const auto record_unsupported = [&](uintptr_t site) {
            const auto& rewrite = rewrite_sites.at(site);
            if (rewrite.protect_red_zone) {
                ++result.unrelocatable_memory_instruction_count;
                // theatrhythm: THEATRHYTHM_REDZONE=2 lists them (image offsets).
                if (module->verbose) {
                    std::fprintf(stderr, "Red zone: not relocatable: guest 0x%llx (function 0x%llx)\n",
                                 static_cast<unsigned long long>(site - module->image),
                                 static_cast<unsigned long long>(function_start - module->image));
                }
            }
        };
        const auto overlaps_patched_span = [&patched_spans](uintptr_t start, uintptr_t end) {
            return std::ranges::any_of(patched_spans, [start, end](const auto& patched) {
                return start < patched.second && patched.first < end;
            });
        };

        for (const uintptr_t site : unresolved_sites) {
            if (module->patched.contains(reinterpret_cast<u8*>(site))) {
                continue;
            }

            const auto site_instruction = function.instructions.find(site);
            ASSERT(site_instruction != function.instructions.end());
            if (function.has_indirect_branch ||
                site_instruction->second.instruction.length < ShortJumpSize) {
                record_unsupported(site);
                continue;
            }

            const RelocationSpan site_span{
                .instructions = {&site_instruction->second},
                .patch_start = site,
                .continuation = site + site_instruction->second.instruction.length,
                .patch_size = site_instruction->second.instruction.length,
            };
            const size_t trampoline_start = module->trampoline_gen.getSize();
            const auto site_trampoline_offset = emit_span(site_span);
            if (!site_trampoline_offset) {
                record_unsupported(site);
                continue;
            }

            constexpr s64 ShortJumpMin = std::numeric_limits<s8>::min();
            constexpr s64 ShortJumpMax = std::numeric_limits<s8>::max();
            const auto relay_slot = std::ranges::find_if(relay_slots, [site](uintptr_t address) {
                const s64 displacement =
                    static_cast<s64>(address) - static_cast<s64>(site + ShortJumpSize);
                return displacement >= ShortJumpMin && displacement <= ShortJumpMax;
            });
            if (relay_slot != relay_slots.end()) {
                const auto* site_trampoline =
                    module->trampoline_gen.getCode() + *site_trampoline_offset;

                auto& patch_gen = module->patch_gen;
                patch_gen.reset();
                patch_gen.setSize(*relay_slot - reinterpret_cast<uintptr_t>(patch_gen.getCode()));
                patch_gen.jmp(site_trampoline, Xbyak::CodeGenerator::LabelType::T_NEAR);

                patch_gen.reset();
                patch_gen.setSize(site - reinterpret_cast<uintptr_t>(patch_gen.getCode()));
                patch_gen.jmp(reinterpret_cast<void*>(*relay_slot),
                              Xbyak::CodeGenerator::LabelType::T_SHORT);
                patch_gen.nop(site_span.patch_size - ShortJumpSize);

                record_rewrites(site_span);
                patched_spans.emplace_back(site_span.patch_start, site_span.continuation);
                std::erase(short_relay_slots, *relay_slot);
                relay_slots.erase(relay_slot);
                continue;
            }

            const auto find_host = [&](uintptr_t short_jump_address) {
                std::pair<std::optional<RelocationSpan>, std::optional<size_t>> result;
                const s64 minimum_host = static_cast<s64>(short_jump_address) +
                                         static_cast<s64>(ShortJumpSize) -
                                         static_cast<s64>(NearJumpSize) + ShortJumpMin;
                const s64 maximum_host = static_cast<s64>(short_jump_address) +
                                         static_cast<s64>(ShortJumpSize) -
                                         static_cast<s64>(NearJumpSize) + ShortJumpMax;

                auto candidate = function.instructions.lower_bound(
                    static_cast<uintptr_t>(std::max<s64>(minimum_host, 0)));
                for (; candidate != function.instructions.end() &&
                       static_cast<s64>(candidate->first) <= maximum_host;
                     ++candidate) {
                    RelocationSpan span{.patch_start = candidate->first,
                                        .continuation = candidate->first};
                    while (span.patch_size < NearJumpSize * 2) {
                        const auto instruction = function.instructions.find(span.continuation);
                        if (instruction == function.instructions.end() ||
                            (span.continuation != span.patch_start &&
                             function.branch_targets.contains(span.continuation)) ||
                            instruction->second.instruction.meta.category == ZYDIS_CATEGORY_CALL) {
                            span.instructions.clear();
                            break;
                        }
                        span.instructions.push_back(&instruction->second);
                        span.patch_size += instruction->second.instruction.length;
                        span.continuation += instruction->second.instruction.length;
                        if (span.patch_size < NearJumpSize * 2 &&
                            IsControlFlowTerminator(instruction->second.instruction)) {
                            span.instructions.clear();
                            break;
                        }
                    }
                    if (span.instructions.empty() ||
                        !(span.continuation <= site ||
                          span.patch_start >= site_span.continuation) ||
                        overlaps_patched_span(span.patch_start, span.continuation)) {
                        continue;
                    }

                    const uintptr_t relay_address = span.patch_start + NearJumpSize;
                    const s64 displacement = static_cast<s64>(relay_address) -
                                             static_cast<s64>(short_jump_address + ShortJumpSize);
                    if (displacement < ShortJumpMin || displacement > ShortJumpMax) {
                        continue;
                    }

                    const auto offset = emit_span(span);
                    if (!offset) {
                        continue;
                    }
                    result.first = std::move(span);
                    result.second = offset;
                    break;
                }
                return result;
            };

            auto [host_span, host_trampoline_offset] = find_host(site);
            std::optional<uintptr_t> final_relay_slot;
            uintptr_t final_short_jump = site;
            std::map<uintptr_t, uintptr_t> relay_parent{{site, site}};
            std::vector<uintptr_t> relay_queue{site};
            for (size_t queue_index = 0;
                 !host_span && !final_relay_slot && queue_index < relay_queue.size();
                 ++queue_index) {
                const uintptr_t current = relay_queue[queue_index];
                if (current != site) {
                    if (std::ranges::find(relay_slots, current) != relay_slots.end()) {
                        final_relay_slot = current;
                        final_short_jump = relay_parent.at(current);
                        break;
                    }
                    const auto near_slot =
                        std::ranges::find_if(relay_slots, [current](uintptr_t address) {
                            const s64 displacement = static_cast<s64>(address) -
                                                     static_cast<s64>(current + ShortJumpSize);
                            return address != current && displacement >= ShortJumpMin &&
                                   displacement <= ShortJumpMax;
                        });
                    if (near_slot != relay_slots.end()) {
                        final_relay_slot = *near_slot;
                        final_short_jump = current;
                        break;
                    }

                    auto bridge_host = find_host(current);
                    if (bridge_host.first) {
                        host_span = std::move(bridge_host.first);
                        host_trampoline_offset = bridge_host.second;
                        final_short_jump = current;
                        break;
                    }
                }

                for (const uintptr_t slot : short_relay_slots) {
                    if (relay_parent.contains(slot)) {
                        continue;
                    }
                    const s64 displacement =
                        static_cast<s64>(slot) - static_cast<s64>(current + ShortJumpSize);
                    if (displacement < ShortJumpMin || displacement > ShortJumpMax) {
                        continue;
                    }
                    relay_parent.emplace(slot, current);
                    relay_queue.push_back(slot);
                }
            }

            if (!host_span && !final_relay_slot) {
                module->trampoline_gen.setSize(trampoline_start);
                record_unsupported(site);
                continue;
            }

            const auto* site_trampoline =
                module->trampoline_gen.getCode() + *site_trampoline_offset;
            auto& patch_gen = module->patch_gen;
            uintptr_t relay_address{};
            if (final_relay_slot) {
                relay_address = *final_relay_slot;
                patch_gen.reset();
                patch_gen.setSize(relay_address - reinterpret_cast<uintptr_t>(patch_gen.getCode()));
                patch_gen.jmp(site_trampoline, Xbyak::CodeGenerator::LabelType::T_NEAR);
                std::erase(relay_slots, relay_address);
                std::erase(short_relay_slots, relay_address);
            } else {
                ASSERT(host_span && host_trampoline_offset);
                const auto* host_trampoline =
                    module->trampoline_gen.getCode() + *host_trampoline_offset;
                relay_address = host_span->patch_start + NearJumpSize;

                patch_gen.reset();
                patch_gen.setSize(host_span->patch_start -
                                  reinterpret_cast<uintptr_t>(patch_gen.getCode()));
                patch_gen.jmp(host_trampoline, Xbyak::CodeGenerator::LabelType::T_NEAR);
                patch_gen.jmp(site_trampoline, Xbyak::CodeGenerator::LabelType::T_NEAR);
                patch_gen.nop(host_span->patch_size - NearJumpSize * 2);
            }

            uintptr_t jump_target = relay_address;
            while (final_short_jump != site) {
                patch_gen.reset();
                patch_gen.setSize(final_short_jump -
                                  reinterpret_cast<uintptr_t>(patch_gen.getCode()));
                patch_gen.jmp(reinterpret_cast<void*>(jump_target),
                              Xbyak::CodeGenerator::LabelType::T_SHORT);
                jump_target = final_short_jump;
                const auto parent = relay_parent.find(final_short_jump);
                ASSERT(parent != relay_parent.end());
                final_short_jump = parent->second;
                std::erase(relay_slots, jump_target);
                std::erase(short_relay_slots, jump_target);
            }

            patch_gen.reset();
            patch_gen.setSize(site - reinterpret_cast<uintptr_t>(patch_gen.getCode()));
            patch_gen.jmp(reinterpret_cast<void*>(jump_target),
                          Xbyak::CodeGenerator::LabelType::T_SHORT);
            patch_gen.nop(site_span.patch_size - ShortJumpSize);

            if (host_span) {
                record_rewrites(*host_span);
                patched_spans.emplace_back(host_span->patch_start, host_span->continuation);
                if (host_span->patch_size >= NearJumpSize * 3) {
                    relay_slots.push_back(host_span->patch_start + NearJumpSize * 2);
                }
                if (host_span->patch_size >= NearJumpSize * 2 + ShortJumpSize) {
                    short_relay_slots.push_back(host_span->patch_start + NearJumpSize * 2);
                }
            }
            record_rewrites(site_span);
            patched_spans.emplace_back(site_span.patch_start, site_span.continuation);
        }
    }
}

} // namespace

// theatrhythm: C entry point (theatrhythm_gpu.h).
extern "C" int theatrhythm_redzone_patch(unsigned char* image, unsigned long long image_size,
                                         unsigned char* trampolines,
                                         unsigned long long trampolines_size,
                                         const TheatrhythmRedZoneSegment* segments,
                                         unsigned long long segment_count,
                                         TheatrhythmRedZoneResult* result) {
    *result = {};
    Common::Decoder::Instance(); // created here, before the analysis threads use it
    try {
        const char* setting = std::getenv("THEATRHYTHM_REDZONE");
        PatchModule module{image, image_size, trampolines, trampolines_size, setting && setting[0] == '2'};
        for (unsigned long long i = 0; i < segment_count; ++i) {
            const auto& s = segments[i];
            std::vector<uintptr_t> starts(s.function_count);
            for (unsigned long long f = 0; f < s.function_count; ++f) {
                starts[f] = reinterpret_cast<uintptr_t>(image) + s.function_starts[f];
            }
            std::set<uintptr_t> skip;
            for (unsigned long long f = 0; f < s.skip_count; ++f) {
                skip.insert(reinterpret_cast<uintptr_t>(image) + s.skip_starts[f]);
            }
            PatchRedZoneMemoryInstructions(&module, reinterpret_cast<u64>(image) + s.address,
                                           s.size, starts, skip, *result);
        }
        result->trampoline_bytes = module.trampoline_gen.getSize();
        result->trampoline_exhausted = module.trampoline_exhausted;
        return 0;
    } catch (const Xbyak::Error& error) {
        std::fprintf(stderr, "Red zone: code generation failed (%s)\n", Xbyak::ConvertErrorToString(error));
        return -1;
    }
}

#endif // _WIN32
