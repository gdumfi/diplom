#pragma once
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <llvm/IR/Module.h>

#include "solc/report/Finding.h"

namespace solc::report {

// метаданные прогона для шапки контекст-пака (--llm-context)
struct ContextMeta {
    std::string source;        // путь к файлу или дескриптор источника
    std::string solcVersion;   // версия solc, которой собирали
    std::string address;       // 0x… если тянули с Etherscan (иначе пусто)
    int         chain = 0;     // chainId (0 = не из сети)
};

// ContextPackWriter — «сенсор для LLM-аналитика» (Фаза A нейро-символической схемы).
// собирает самодостаточный markdown-пак: факты, извлечённые из EVM-размеченного LLVM IR
// (слоты storage, внешние вызовы, owner-гварды, оракулы, поток ether), уже найденные
// детекторами зацепки и гипотезы-хуки под категории Solodit/SWC. предназначен для чтения
// Claude Code в интерактивной сессии — заземляет рассуждение о логических/экономических
// багах на ТОЧНЫХ фактах компилятора, а не на пересказе текста (лекарство от галлюцинаций).
class ContextPackWriter {
public:
    // модули НЕ мутируются; ожидается, что promoteLocals уже прогонялся анализатором
    // (loads/stores в addrspace(1) переживают mem2reg — это не allocas).
    void write(const std::vector<std::unique_ptr<llvm::Module>>& mods,
               const Findings& findings,
               const std::vector<std::pair<std::string, std::string>>& files,
               const ContextMeta& meta,
               std::ostream& os);
};

}  // namespace solc::report
