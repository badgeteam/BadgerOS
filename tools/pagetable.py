#!/usr/bin/env python3

import os
from argparse import *
from mmap import *
from elftools.elf.elffile import ELFFile


def read_word(mem, paddr: int) -> int:
    if type(mem) == ELFFile:
        for i in range(mem.num_segments()):
            seg = mem.get_segment(i)
            if seg.header.p_type != 'PT_LOAD': continue
            if paddr < seg.header.p_paddr: continue
            if paddr + 8 > seg.header.p_paddr + seg.header.p_memsz: continue
            mem.stream.seek(paddr - seg.header.p_paddr + seg.header.p_offset)
            tmp = mem.stream.read(8)
            val = 0
            for i in range(8):
                val |= tmp[i] << (i*8)
            return val
        raise IndexError(f"paddr {paddr:x} out of range")
    elif type(mem) == mmap or type(mem) == bytearray or type(mem) == bytes:
        val = 0
        for i in range(8):
            val |= mem[i+paddr] << (i*8)
        return val
    else:
        mem.seek(paddr, os.SEEK_SET)
        tmp = fd.read(8)
        val = 0
        for i in range(8):
            val |= tmp[i] << (i*8)
        return val


class Flags:
    def __init__(self, packed: int):
        self.unpack(packed)
    
    def unpack(self, packed: int):
        self.v    = bool((packed >> 0) & 1)
        self.r    = bool((packed >> 1) & 1)
        self.w    = bool((packed >> 2) & 1)
        self.x    = bool((packed >> 3) & 1)
        self.u    = bool((packed >> 4) & 1)
        self.g    = bool((packed >> 5) & 1)
        self.a    = bool((packed >> 6) & 1)
        self.pbmt = (packed >> 61) & 3
        self.n    = bool((packed >> 63) & 1)
    
    def pack(self) -> bool:
        packed = 0
        packed |= int(self.v) << 0
        packed |= int(self.r) << 1
        packed |= int(self.w) << 2
        packed |= int(self.x) << 3
        packed |= int(self.u) << 4
        packed |= int(self.g) << 5
        packed |= int(self.a) << 6
        packed |= self.pbmt   << 61
        packed |= int(self.n) << 63
        return packed
    
    def __repr__(self) -> str:
        return f"Flags(0x{self.pack():016x})"
    
    def __str__(self) -> str:
        res = ""
        res += "v" if self.v else "-"
        res += "r" if self.r else "-"
        res += "w" if self.w else "-"
        res += "x" if self.x else "-"
        res += "u" if self.u else "-"
        res += "g" if self.g else "-"
        res += "a" if self.a else "-"
        res += "n" if self.n else "-"
        pbmt = ["PMA", "NC", "IO", "3"]
        res += f" pbmt:{pbmt[self.pbmt]:3s}"
        return res


class PTE(Flags):
    def __init__(self, packed: int):
        self.unpack(packed)
        self.page: Page | PageTable = None
    
    def leaf(self) -> bool:
        return self.r or self.w or self.x
    
    def flags(self) -> Flags:
        return Flags(self.pack())
    
    def unpack(self, packed: int):
        Flags.unpack(self, packed)
        self.rsw  = (packed >> 8) & 3
        self.ppn  = (packed >> 10) & 0xFFFFFFFFFFF
    
    def pack(self) -> int:
        packed = Flags.pack(self)
        packed |= self.rsw    << 8
        packed |= self.ppn    << 10
        return packed
    
    def paddr(self) -> int:
        return self.ppn << 12
    
    def length(self, level: int) -> int:
        return 1 << (12 + 9 * level)
    
    def __repr__(self) -> str:
        return f"PTE(0x{self.pack():016x})"
    
    def __str__(self) -> str:
        res = ""
        res += "v" if self.v else "-"
        res += "r" if self.r else "-"
        res += "w" if self.w else "-"
        res += "x" if self.x else "-"
        res += "u" if self.u else "-"
        res += "g" if self.g else "-"
        res += "a" if self.a else "-"
        res += "n" if self.n else "-"
        pbmt = ["PMA", "NC", "IO", "3"]
        res += f" rsw:{self.rsw:d} ppn:0x{self.ppn:011x} pbmt:{pbmt[self.pbmt]:3s}"
        return res


class Page:
    def __init__(self, vaddr: int, paddr: int, length: int):
        self.vaddr  = vaddr
        self.paddr  = paddr
        self.length = length
        assert self.vaddr % self.length == 0
        if self.paddr % self.length != 0 and self.paddr % 4096 == 0:
            print(f"WARNING: Superpage 0x{self.paddr:x} misaligned (0x{self.length:x})")
        assert type(self.length) == int
    
    def __repr__(self) -> str:
        return f"Page(0x{self.paddr:016x}, 0x{self.length:016x})"
    
    def __str__(self) -> str:
        return f"va:{self.vaddr:016x} pa:{self.paddr:016x} len:{self.length:016x}"


class PageTable(Page):
    def __init__(self, vaddr: int, paddr: int, level: int, memory: bytearray, root = False):
        self.length  = 4096
        self.vaddr   = vaddr
        self.paddr   = paddr
        self.root    = root
        assert 0 <= level <= 4
        assert self.vaddr % self.length == 0
        assert self.paddr % self.length == 0
        self.entries = None
        if memory:
            self.unpack(vaddr, paddr, memory, level, root)
    
    def unpack(self, vaddr: int, paddr: int, memory: bytearray, level: int, root = False):
        self.vaddr   = vaddr
        self.paddr   = paddr
        self.root    = root
        assert 0 <= level <= 4
        assert self.vaddr % self.length == 0
        assert self.paddr % self.length == 0
        self.level = level
        self.entries = [PTE(0) for i in range(512)]
        for i in range(512):
            paddr = self.paddr + i * 8
            try:
                self.entries[i] = PTE(read_word(mem, paddr))
            except IndexError as e:
                print(f"IndexError: {', '.join(e.args)}")
                break
        for i in range(512):
            pt_vpn = vaddr | (i << (12+9*level))
            if self.root and i & 256:
                pt_vpn |= (0xfffffffffffff000 << (9*level)) & 0xffffffffffffffff
            pt_ppn = self.entries[i].ppn
            if self.entries[i].n:
                pass
            elif self.entries[i].v and self.entries[i].leaf():
                self.entries[i].page = Page(pt_vpn, pt_ppn << 12, 1 << (12+9*level))
            elif self.entries[i].v:
                if level == 0:
                    print(f"WARNING: Non-leaf PTE at level 0: {self.entries[i]}")
                    self.entries[i].page = None
                else:
                    self.entries[i].page = PageTable(pt_vpn, pt_ppn << 12, level-1, memory)
    
    def __repr__(self) -> str:
        return f"PageTable(0x{self.vaddr:016x}, 0x{self.paddr:016x}, {self.level:d}, <...>)"
    
    def __str__(self, indent=0, indent_type="  ", g=False):
        res = f"{indent_type*indent}{'-g'[g]} pa:{self.paddr:016x} len:{self.length:016x}\n"
        indent += 1
        for pte in self.entries:
            if pte.v and type(pte.page) == Page:
                res += indent_type*indent
                res += Flags.__str__(pte) + f" rsw:{pte.rsw:d} " + str(pte.page) + '\n'
            elif pte.v and type(pte.page) == PageTable:
                res += indent_type*indent
                res += str(pte) + '\n'
                res += pte.page.__str__(indent+1, indent_type, pte.g) + '\n'
            elif pte.v:
                res += indent_type*indent
                res += str(pte) + '\n'
        return res[:-1]


class Memmap:
    def __init__(self, pt: PageTable):
        self.regions: list[tuple[Flags, Page]] = []
        vaddr  = -1
        paddr  = -1
        pflags = 0
        def walk(pt: PageTable):
            nonlocal vaddr, paddr, pflags
            for pte in pt.entries:
                if type(pte.page) == PageTable:
                    walk(pte.page)
                if type(pte.page) != Page:
                    continue
                if pte.page.vaddr == vaddr and pte.page.paddr == paddr and pte.flags().pack() == pflags:
                    self.regions[-1][1].length += pte.page.length
                    vaddr += pte.page.length
                    paddr += pte.page.length
                else:
                    self.regions.append((pte.flags(), Page(pte.page.vaddr, pte.page.paddr, pte.page.length)))
                    vaddr  = pte.page.vaddr + pte.page.length
                    paddr  = pte.page.paddr + pte.page.length
                    pflags = pte.flags().pack()
        walk(pt)
    
    def __str__(self):
        res = ""
        for r in self.regions:
            res += str(r[0]) + ' ' + str(r[1]) + '\n'
        return res[:-1]



if __name__ == "__main__":
    parser = ArgumentParser()
    parser.add_argument("file",                      action="store",                help="Binary file to read page table from")
    parser.add_argument("--pt_root", "--root", "-p", action="store", required=True, help="Page table root physical page number")
    parser.add_argument("--levels", "-l",            action="store", required=True, help="Level of the page table being dumped (0-4)")
    parser.add_argument("--raw", "-r",               action="store_true",           help="Interpret as a physical memory dump, not an ELF file")
    args = parser.parse_args()
    if args.pt_root.lower().startswith("0x"):
        args.pt_root = args.pt_root[2:]
    fd   = open(args.file, "rb")
    if args.raw:
        fd.seek(0, os.SEEK_END)
        size = fd.tell()
        fd.seek(0, os.SEEK_SET)
        mem = mmap(fd.fileno(), size, prot=PROT_READ, flags=MAP_PRIVATE)
    else:
        mem = ELFFile(fd)
        for i in range(mem.num_segments()):
            seg = mem.get_segment(i)
            print(f"{seg.header.p_type:8}  paddr {seg.header.p_paddr:016x} len {seg.header.p_memsz:016x}")
    lvl  = int(args.levels)
    pt   = PageTable(0, int(args.pt_root, 16) << 12, lvl, mem, True)
    mm   = Memmap(pt)
    print(str(mm))
