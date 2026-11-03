`stb_image.h` v2.30 and `stb_image_write.h` v1.16 from https://github.com/nothings/stb at commit
d14d46f6b44456eb88beac58cba833a4135d4c89 (MIT or public domain, at the end of each file). The DLC
maker's core (`tools/dlcmaker/core/image.cpp`, which holds their implementations) reads the project's
pictures and writes PNGs with them; its Linux window (`tools/dlcmaker/app/main_linux.cpp`) reads its
logo previews and monster pictures through the same copy.
