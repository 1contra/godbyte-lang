#include "../include/emitter.hpp"
#include "x86_encoder.hpp"
#include <cstring>

namespace gbpp {
#pragma pack(push, 1)
    struct CoffHeader {
        uint16_t machine = 0x8664;
        uint16_t numSections = 0;
        uint32_t timeDateStamp = 0;
        uint32_t pointerToSymbolTable = 0;
        uint32_t numberOfSymbols = 0;
        uint16_t sizeOfOptionalHeader = 0;
        uint16_t characteristics = 0;
    };
    struct SectionHeader {
        char name[8] = { 0 };
        uint32_t virtualSize = 0; uint32_t virtualAddress = 0;
        uint32_t sizeOfRawData = 0; uint32_t pointerToRawData = 0;
        uint32_t pointerToRelocations = 0; uint32_t pointerToLinenumbers = 0;
        uint16_t numberOfRelocations = 0; uint16_t numberOfLinenumbers = 0;
        uint32_t characteristics = 0;
    };
    struct Relocation { uint32_t va; uint32_t symIdx; uint16_t type; };
    struct SymbolEntry {
        union { char shortName[8]; struct { uint32_t zeroes; uint32_t offset; } longName; } name;
        uint32_t value = 0; int16_t sectionNumber = 0;
        uint16_t type = 0; uint8_t storageClass = 0; uint8_t numberOfAuxSymbols = 0;
    };
#pragma pack(pop)

    class CoffEmitter : public Emitter {
        BinaryBuffer textBuf;
        BinaryBuffer dataBuf;
        BinaryBuffer stringTab;
        BinaryBuffer* curBuf = &textBuf;

        std::map<std::string, size_t> labels;
        std::vector<SymbolEntry> symbols;
        std::map<std::string, uint32_t> symLookup;
        std::vector<Relocation> textRelocs;

        uint32_t addStr(const std::string& s) {
            if (stringTab.size() == 0) stringTab.emit32(4);
            uint32_t off = stringTab.size();
            for (char c : s) stringTab.emit8(c); stringTab.emit8(0);
            stringTab.patch32(0, stringTab.size());
            return off;
        }

        uint32_t getOrAddSym(const std::string& name, int16_t sec, uint8_t sclass, uint32_t val = 0) {
            if (symLookup.count(name)) return symLookup[name];
            SymbolEntry sym; std::memset(&sym, 0, sizeof(sym));
            sym.sectionNumber = sec; sym.storageClass = sclass; sym.value = val;
            if (name.length() <= 8) std::memcpy(sym.name.shortName, name.c_str(), name.length());
            else { sym.name.longName.zeroes = 0; sym.name.longName.offset = addStr(name); }
            uint32_t idx = symbols.size();
            symbols.push_back(sym);
            symLookup[name] = idx;
            return idx;
        }

    public:
        void enterTextSection() override { curBuf = &textBuf; }
        void enterDataSection() override { curBuf = &dataBuf; }
        void emitGlobal(const std::string& name) override { getOrAddSym(name, 1, 2); }
        void emitExtern(const std::string& name) override { getOrAddSym(name, 0, 2); }
        void emitDataString(const std::string& l, const std::string& s) override {
            getOrAddSym(l, 2, 3, dataBuf.size());
            for (char c : s) dataBuf.emit8(c); dataBuf.emit8(0);
        }
        void emitLabel(const std::string& l) override { labels[l] = curBuf->size(); }
        void emitInstruction(const MachineInstr& inst) override { X86Encoder::encode(inst, *curBuf); }

        void finalize(std::ostream& out) override {
            for (auto& [l, off] : labels) if (symLookup.count(l)) symbols[symLookup[l]].value = off;

            for (const auto& fix : textBuf.fixups) {
                if (labels.count(fix.symbol)) {
                    int32_t rel = labels[fix.symbol] - (fix.offset + 4);
                    textBuf.patch32(fix.offset, rel);
                }
                else {
                    textRelocs.push_back({ (uint32_t)fix.offset, symLookup[fix.symbol], 0x0004 });
                }
            }

            CoffHeader h; h.numSections = (dataBuf.size() > 0) ? 2 : 1; h.numberOfSymbols = symbols.size();
            SectionHeader ts; std::memcpy(ts.name, ".text", 5); ts.sizeOfRawData = textBuf.size(); ts.characteristics = 0x60500020;
            SectionHeader ds; std::memcpy(ds.name, ".data", 5); ds.sizeOfRawData = dataBuf.size(); ds.characteristics = 0xC0300040;

            uint32_t off = sizeof(CoffHeader) + (sizeof(SectionHeader) * h.numSections);
            ts.pointerToRawData = off; off += textBuf.size();
            if (dataBuf.size() > 0) { ds.pointerToRawData = off; off += dataBuf.size(); }
            if (!textRelocs.empty()) { ts.pointerToRelocations = off; ts.numberOfRelocations = textRelocs.size(); off += textRelocs.size() * 10; }
            h.pointerToSymbolTable = off;

            out.write((char*)&h, sizeof(h)); out.write((char*)&ts, sizeof(ts));
            if (dataBuf.size() > 0) out.write((char*)&ds, sizeof(ds));
            out.write((char*)textBuf.bytes(), textBuf.size());
            if (dataBuf.size() > 0) out.write((char*)dataBuf.bytes(), dataBuf.size());
            if (!textRelocs.empty()) out.write((char*)textRelocs.data(), textRelocs.size() * 10);
            out.write((char*)symbols.data(), symbols.size() * 18);
            if (stringTab.size() > 0) out.write((char*)stringTab.bytes(), stringTab.size());
            else { uint32_t zero = 4; out.write((char*)&zero, 4); }
        }
    };
    std::unique_ptr<Emitter> Emitter::createCoffWin64() {
        return std::make_unique<CoffEmitter>();
    }
}