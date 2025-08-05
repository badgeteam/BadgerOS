#!/usr/bin/env python3

import sys, os

if len(sys.argv) != 4:
    print("Usage: ramfs-gen.py [indir] [outfile] [name]")
    exit(1)

outfd = open(sys.argv[2], "w")

outfd.write("// WARNING: This is a generated file, do not edit it!\n")
outfd.write("// clang-format off\n")
outfd.write("// NOLINTBEGIN\n")
outfd.write("#include <stdint.h>\n")
outfd.write("#include <stddef.h>\n")
outfd.write("#include \"filesystem.h\"\n")
outfd.write("#include \"assertions.h\"\n")

roms=""
dirs=""
files=""
file_count=0



def escape(raw: str):
    return raw\
        .replace('\\', '\\\\')\
        .replace('\r', '\\r')\
        .replace('\n', '\\n')\
        .replace('"', '\\"')


def add_rom(path, virtpath, name):
    global roms, files
    infd = open(path, "rb")
    data = infd.read()
    roms += "uint8_t const {}[] = {{\n    ".format(name)
    for byte in data:
        roms += "0x{:02x},".format(byte)
    roms += "\n};\n"
    roms += "size_t const {}_len = {};\n".format(name, len(data))
    files += "    fd = fs_open(FILE_NONE, \"{}\", {}, FS_O_CREATE | FS_O_WRITE_ONLY).file;\n".format(escape(virtpath), len(virtpath))
    files += "    assert_always(fd.metadata);\n"
    files += "    len = fs_write(fd, {}, {}_len);\n".format(name, name)
    files += "    assert_always(len == {}_len);\n".format(name)
    files += "    fs_file_drop(fd);\n"
    infd.close()


def add_dir(path, virtpath):
    global dirs, file_count
    for filename in os.listdir(path):
        if os.path.isdir(path + "/" + filename):
            dirs += "    assert_dev_keep(fs_make_file(FILE_NONE, \"{}\", {}, (make_file_spec_t){{.type = NODE_TYPE_DIRECTORY}}) >= 0);\n".format(escape(virtpath + "/" + filename), len(virtpath) + 1 + len(filename))
            add_dir(path + "/" + filename, virtpath + "/" + filename)
        else:
            add_rom(path + "/" + filename, virtpath + "/" + filename, "filerom_{}".format(file_count))
            file_count += 1


add_dir(sys.argv[1], "")
outfd.write(roms)
outfd.write("void {}() {{\n".format(sys.argv[3]))
outfd.write("    file_t fd;\n")
outfd.write("    errno64_t len;\n")
outfd.write(dirs)
outfd.write(files)
outfd.write("    (void)len;\n")
outfd.write("}\n")
outfd.write("// NOLINTEND\n")
outfd.flush()
outfd.close()
