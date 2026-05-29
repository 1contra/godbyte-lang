#include "../include/emitter.hpp"
#include "x86_encoder.hpp"
#include <cstring>

namespace gbpp {
    struct Elf64_Ehdr {
        uint8_t e_ident[16] = { 0x7f,'E','L','F',2,1,1,0,0,0,0,0,0,0,0,0 };
        uint16_t e_type = 1; uint16_t e_machine = 0x3E; uint32_t e_version = 1;
        uint64_t e_entry = 0; uint64_t e_phoff = 0; uint64_t e_shoff = 64;
        uint32_t e_flags = 0; uint16_t e_ehsize = 64; uint16_t e_phentsize = 0;
        uint16_t e_phnum = 0; uint16_t e_shentsize = 64; uint16_t e_shnum = 6;
        uint16_t e_shstrndx = 2;
    };
    struct Elf64_Shdr {
        uint32_t sh_name = 0; uint32_t sh_type = 0; uint64_t sh_flags = 0;
        uint64_t sh_addr = 0; uint64_t sh_offset = 0; uint64_t sh_size = 0;
        uint32_t sh_link = 0; uint32_t sh_info = 0; uint64_t sh_addralign = 0; uint64_t sh_entsize = 0;
    };
    struct Elf64_Sym { uint32_t st_name; uint8_t st_info; uint8_t st_other; uint16_t st_shndx; uint64_t st_value; uint64_t st_size; };
    struct Elf64_Rela { uint64_t r_offset; uint64_t r_info; int64_t r_addend; };

    class ElfEmitter : public Emitter {
        BinaryBuffer textBuf, dataBuf, strTab;
        BinaryBuffer* curBuf = &textBuf;
        std::map<std::string, size_t> labels;
        std::vector<Elf64_Sym> symbols;
        std::map<std::string, uint32_t> symLookup;
        std::vector<Elf64_Rela> relocs;

        uint32_t addStr(const std::string& s) {
            if (strTab.size() == 0) strTab.emit8(0);
            uint32_t off = strTab.size();
            for (char c : s) strTab.emit8(c); strTab.emit8(0);
            return off;
        }

        uint32_t getOrAddSym(const std::string& n, uint16_t shndx, uint8_t bindType, uint64_t val = 0) {
            if (symLookup.count(n)) return symLookup[n];
            Elf64_Sym s; std::memset(&s, 0, sizeof(s));
            s.st_name = addStr(n); s.st_info = bindType; s.st_shndx = shndx; s.st_value = val;
            uint32_t idx = symbols.size(); symbols.push_back(s); symLookup[n] = idx;
            return idx;
        }

    public:
        ElfEmitter() {
            Elf64_Sym nullSym; std::memset(&nullSym, 0, sizeof(nullSym)); symbols.push_back(nullSym);
        }
        void enterTextSection() override { curBuf = &textBuf; }
        void enterDataSection() override { curBuf = &dataBuf; }
        void emitGlobal(const std::string& name) override { getOrAddSym(name, 1, 0x10); }
        void emitExtern(const std::string& name) override { getOrAddSym(name, 0, 0x10); }
        void emitDataString(const std::string& l, const std::string& s) override {
            getOrAddSym(l, 4, 0x00, dataBuf.size());
            for (char c : s) dataBuf.emit8(c); dataBuf.emit8(0);
        }
        void emitLabel(const std::string& l) override { labels[l] = curBuf->size(); }
        void emitInstruction(const MachineInstr& inst) override { X86Encoder::encode(inst, *curBuf); }

        void finalize(std::ostream& out) override {
            for (auto& [l, off] : labels) if (symLookup.count(l)) symbols[symLookup[l]].st_value = off;

            for (const auto& fix : textBuf.fixups) {
                if (labels.count(fix.symbol)) {
                    int32_t rel = labels[fix.symbol] - (fix.offset + 4);
                    textBuf.patch32(fix.offset, rel);
                }
                else {
                    Elf64_Rela rela; rela.r_offset = fix.offset;
                    rela.r_info = ((uint64_t)symLookup[fix.symbol] << 32) | 2;
                    rela.r_addend = -4; relocs.push_back(rela);
                }
            }

            const char* shstrs = "\0.text\0.shstrtab\0.symtab\0.strtab\0.data\0.rela.text\0";
            uint32_t shstrsz = 50;

            Elf64_Ehdr ehdr; ehdr.e_shnum = 7; ehdr.e_shstrndx = 2;
            std::vector<Elf64_Shdr> shdrs(7);

            uint64_t offset = sizeof(Elf64_Ehdr) + (7 * sizeof(Elf64_Shdr));

            shdrs[1] = { 1, 1, 6, 0, offset, textBuf.size(), 0, 0, 16, 0 }; offset += textBuf.size(); // .text
            shdrs[2] = { 7, 3, 0, 0, offset, shstrsz, 0, 0, 1, 0 }; offset += shstrsz; // .shstrtab
            shdrs[3] = { 17, 2, 0, 0, offset, symbols.size() * 24, 4, 1, 8, 24 }; offset += symbols.size() * 24; // .symtab
            shdrs[4] = { 25, 3, 0, 0, offset, strTab.size(), 0, 0, 1, 0 }; offset += strTab.size(); // .strtab
            shdrs[5] = { 33, 1, 3, 0, offset, dataBuf.size(), 0, 0, 4, 0 }; offset += dataBuf.size(); // .data
            shdrs[6] = { 39, 4, 0, 0, offset, relocs.size() * 24, 3, 1, 8, 24 }; // .rela.text

            out.write((char*)&ehdr, sizeof(ehdr)); out.write((char*)shdrs.data(), shdrs.size() * sizeof(Elf64_Shdr));
            out.write((char*)textBuf.bytes(), textBuf.size());
            out.write(shstrs, shstrsz);
            out.write((char*)symbols.data(), symbols.size() * 24);
            out.write((char*)strTab.bytes(), strTab.size());
            if (dataBuf.size() > 0) out.write((char*)dataBuf.bytes(), dataBuf.size());
            if (!relocs.empty()) out.write((char*)relocs.data(), relocs.size() * 24);
        }
    };
    std::unique_ptr<Emitter> Emitter::createElfSysV() {
        return std::make_unique<ElfEmitter>();
    }
}