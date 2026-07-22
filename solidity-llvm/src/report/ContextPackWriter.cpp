#include "solc/report/ContextPackWriter.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>

#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>

#include "solc/analyzer/IRTrace.h"

namespace solc::report {

namespace {

namespace trace = solc::analyzer::trace;

// факты одной функции, извлечённые из EVM-размеченного IR (ground truth для LLM-аналитика)
struct FnFacts {
    std::string name;
    std::vector<std::string> abiParams;   // ABI-типы параметров из !scsa.abi
    bool internal = false;                // !scsa.internal — не public-точка входа
    bool unmodeled = false;               // !scsa.unmodeled_modifier — гвард мог остаться в модификаторе
    std::set<int> slotsRead, slotsWritten;
    // запись в СКАЛЯРНЫЙ слот (не через map_elem): кандидат в привилегированное состояние
    // (owner/paused/rate). запись в map[msg.sender] сюда НЕ попадает — это per-user, не привилегия
    std::set<int> slotsWrittenScalar;
    int externalCalls = 0;
    std::set<std::string> callKinds;      // call | delegatecall | staticcall
    bool readsMsgValue = false;           // принимает ether (payable-путь)
    bool readsMsgSender = false;
    bool readsTxOrigin = false;
    bool usesBlockCtx = false;            // block.timestamp/number — слабая рандомность/дедлайны
    bool usesMapElem = false;             // per-user mapping-балансы
    bool selfdestruct = false;
    bool opaqueStore = false;             // запись в непрозрачный слот после вызова
    bool oracleRead = false;              // spot-цена из DEX-резерва
    int  reverts = 0;                     // число require/revert-веток
    bool authGate = false;                // в теле есть проверка msg.sender, гейтящая revert
    int  firstLine = -1;
};

std::string joinInts(const std::set<int>& s) {
    std::string out;
    for (int v : s) { if (!out.empty()) out += ", "; out += std::to_string(v); }
    return out.empty() ? "—" : out;
}

std::string joinStrs(const std::set<std::string>& s) {
    std::string out;
    for (const auto& v : s) { if (!out.empty()) out += ", "; out += v; }
    return out.empty() ? "—" : out;
}

std::string yn(bool b) { return b ? "да" : "нет"; }

// проход по одной функции: собираем все факты за один обход инструкций
FnFacts collectFn(const llvm::Function& F) {
    FnFacts ff;
    ff.name = F.getName().str();
    ff.abiParams = trace::functionParamAbiTypes(F);
    ff.internal = F.getMetadata("scsa.internal") != nullptr;
    ff.unmodeled = F.getMetadata("scsa.unmodeled_modifier") != nullptr;
    ff.authGate = trace::hasMsgSenderGate(F);

    for (const auto& BB : F) {
        for (const auto& I : BB) {
            if (ff.firstLine < 0) {
                if (const llvm::DebugLoc& dl = I.getDebugLoc()) ff.firstLine = dl.getLine();
            }
            // storage load/store в addrspace(1) — слот из !scsa.storage
            if (const auto* ld = llvm::dyn_cast<llvm::LoadInst>(&I)) {
                if (ld->getPointerAddressSpace() == 1) {
                    int s = trace::storageSlotOf(&I);
                    if (s >= 0) ff.slotsRead.insert(s);
                }
            } else if (const auto* st = llvm::dyn_cast<llvm::StoreInst>(&I)) {
                if (st->getPointerAddressSpace() == 1) {
                    int s = trace::storageSlotOf(&I);
                    if (s >= 0) {
                        ff.slotsWritten.insert(s);
                        // указатель прямо от @scsa.map_elem → это запись per-user балансу,
                        // не привилегированному скаляру. иначе (inttoptr/null) — скаляр
                        const llvm::Value* ptr = st->getPointerOperand();
                        bool isMap = false;
                        if (const auto* pc = llvm::dyn_cast<llvm::CallInst>(ptr))
                            if (pc->getCalledFunction() &&
                                pc->getCalledFunction()->getName() == "scsa.map_elem")
                                isMap = true;
                        if (!isMap) ff.slotsWrittenScalar.insert(s);
                    }
                }
            } else if (const auto* call = llvm::dyn_cast<llvm::CallInst>(&I)) {
                const llvm::Function* callee = call->getCalledFunction();
                if (!callee) continue;   // косвенный вызов — пропускаем
                llvm::StringRef n = callee->getName();
                if (n == "scsa.external_call") {
                    ff.externalCalls++;
                    if (auto* md = call->getMetadata("scsa.call.kind"))
                        if (md->getNumOperands() > 0)
                            if (auto* ms = llvm::dyn_cast<llvm::MDString>(md->getOperand(0)))
                                ff.callKinds.insert(ms->getString().str());
                } else if (n == "evm.msg_value")       ff.readsMsgValue = true;
                else if (n == "evm.msg_sender")        ff.readsMsgSender = true;
                else if (n == "evm.tx_origin")         ff.readsTxOrigin = true;
                else if (n == "evm.selfdestruct")      ff.selfdestruct = true;
                else if (n == "evm.revert")            ff.reverts++;
                else if (n == "evm.block_timestamp" ||
                         n == "evm.block_number")      ff.usesBlockCtx = true;
                else if (n == "scsa.map_elem")         ff.usesMapElem = true;
                else if (n == "scsa.opaque_store")     ff.opaqueStore = true;
                else if (n == "scsa.oracle_read")      ff.oracleRead = true;
            }
        }
    }
    // external_call без явного kind — по контракту это обычный "call"
    if (ff.externalCalls > 0 && ff.callKinds.empty()) ff.callKinds.insert("call");
    return ff;
}

}  // namespace

void ContextPackWriter::write(
    const std::vector<std::unique_ptr<llvm::Module>>& mods,
    const Findings& findings,
    const std::vector<std::pair<std::string, std::string>>& files,
    const ContextMeta& meta,
    std::ostream& os) {

    // ---- собираем факты по всем функциям всех модулей ----
    std::vector<FnFacts> fns;
    std::vector<std::string> contractNames;
    for (const auto& mod : mods) {
        contractNames.push_back(mod->getName().str());
        for (const llvm::Function& F : *mod) {
            if (F.isDeclaration()) continue;      // интринсики не анализируем
            fns.push_back(collectFn(F));
        }
    }
    // карта storage: слот -> кто пишет / кто читает
    std::map<int, std::set<std::string>> slotWriters, slotReaders;
    for (const auto& ff : fns) {
        for (int s : ff.slotsWritten) slotWriters[s].insert(ff.name);
        for (int s : ff.slotsRead)    slotReaders[s].insert(ff.name);
    }

    // ---- шапка ----
    os << "# SCSA Context Pack\n\n";
    os << "> Машинно-собранный контекст для нейро-символического анализа (Claude Code — аналитик,\n";
    os << "> SCSA — сенсор). Факты ниже извлечены детерминированным проходом по EVM-размеченному\n";
    os << "> LLVM IR: это **ground truth**, а не пересказ текста. Правило: ни одну гипотезу о баге\n";
    os << "> не считать подтверждённой без PoC / KLEE / нарушенного инварианта (лекарство от\n";
    os << "> галлюцинаций LLM-аудиторов).\n\n";

    os << "## Мета\n\n";
    os << "- источник: `" << (meta.source.empty() ? "—" : meta.source) << "`\n";
    if (!meta.address.empty())
        os << "- адрес: `" << meta.address << "` (chain " << meta.chain << ")\n";
    os << "- solc: `" << meta.solcVersion << "`\n";
    os << "- контрактов (модулей): " << contractNames.size() << " — ";
    for (size_t i = 0; i < contractNames.size(); ++i)
        os << "`" << contractNames[i] << "`" << (i + 1 < contractNames.size() ? ", " : "");
    os << "\n- функций (не-объявлений): " << fns.size() << "\n\n";

    // ---- зацепки от детекторов ----
    os << "## Зацепки детекторов SCSA (детерминированно найдено)\n\n";
    if (findings.empty()) {
        os << "_Детекторы паттернов ничего не нашли._ Это не значит «чисто»: детекторы покрывают\n";
        os << "reentrancy / access-control / oracle / tx.origin / selfdestruct / delegatecall.\n";
        os << "Логика/экономика (округление, share-inflation, неверные инварианты) — вне их охвата,\n";
        os << "смотри раздел «Гипотезы».\n\n";
    } else {
        os << "| severity | rule | функция | строка | слот | описание |\n";
        os << "|---|---|---|---|---|---|\n";
        for (const auto& f : findings) {
            os << "| " << f.severity << " | " << f.rule << " | `" << f.function << "` | "
               << f.line << " | " << f.storageSlot << " | " << f.message << " |\n";
        }
        os << "\n";
    }

    // ---- факты по функциям ----
    os << "## Факты по функциям (из IR)\n\n";
    os << "Легенда: `ext` — внешние вызовы, `$in` — принимает ether (msg.value), `auth` — в теле\n";
    os << "есть проверка msg.sender, гейтящая revert.\n\n";
    os << "| функция | параметры (ABI) | пишет слоты | читает слоты | ext | $in | auth | прочее |\n";
    os << "|---|---|---|---|---|---|---|---|\n";
    for (const auto& ff : fns) {
        std::string params;
        for (size_t i = 0; i < ff.abiParams.size(); ++i)
            params += ff.abiParams[i] + (i + 1 < ff.abiParams.size() ? "," : "");
        if (params.empty()) params = "—";
        std::string extra;
        auto add = [&](const std::string& t) { if (!extra.empty()) extra += " "; extra += t; };
        if (ff.internal)      add("internal");
        if (ff.unmodeled)     add("⚠unmodeled-mod");
        if (!ff.callKinds.empty() && ff.externalCalls > 0) add("kind=" + joinStrs(ff.callKinds));
        if (ff.usesMapElem)   add("map[]");
        if (ff.oracleRead)    add("oracle-spot");
        if (ff.selfdestruct)  add("selfdestruct");
        if (ff.readsTxOrigin) add("tx.origin");
        if (ff.usesBlockCtx)  add("block.*");
        if (ff.opaqueStore)   add("opaque-store");
        if (ff.reverts > 0)   add("require×" + std::to_string(ff.reverts));
        if (extra.empty()) extra = "—";
        os << "| `" << ff.name << "` | " << params << " | " << joinInts(ff.slotsWritten)
           << " | " << joinInts(ff.slotsRead) << " | " << ff.externalCalls << " | "
           << yn(ff.readsMsgValue) << " | " << yn(ff.authGate) << " | " << extra << " |\n";
    }
    os << "\n";

    // ---- карта storage ----
    os << "## Карта storage (слот → доступ)\n\n";
    if (slotWriters.empty() && slotReaders.empty()) {
        os << "_Скалярных storage-слотов не обнаружено._\n\n";
    } else {
        os << "| слот | пишут | читают |\n|---|---|---|\n";
        std::set<int> allSlots;
        for (auto& [s, _] : slotWriters) allSlots.insert(s);
        for (auto& [s, _] : slotReaders) allSlots.insert(s);
        for (int s : allSlots) {
            os << "| " << s << " | " << joinStrs(slotWriters.count(s) ? slotWriters[s] : std::set<std::string>{})
               << " | " << joinStrs(slotReaders.count(s) ? slotReaders[s] : std::set<std::string>{}) << " |\n";
        }
        os << "\n";
    }

    // ---- гипотезы (хуки под Solodit/SWC) ----
    os << "## Гипотезы для проверки (хуки под категории Solodit / SWC)\n\n";
    os << "Каждый пункт привязан к факту выше. Проверяй адресно: сформулируй инвариант и сломай\n";
    os << "его фаззером (forge/medusa) ИЛИ подтверди путь символьно (KLEE) ИЛИ напиши PoC.\n\n";

    std::vector<std::string> hyps;
    // 1. reentrancy/CEI
    for (const auto& ff : fns)
        if (ff.externalCalls > 0 && !ff.slotsWritten.empty())
            hyps.push_back("**Reentrancy / CEI** — `" + ff.name + "` делает внешний вызов И пишет "
                "storage [" + joinInts(ff.slotsWritten) + "]. Проверь порядок checks-effects-interactions "
                "и cross-function reentrancy (общий слот с другой публичной функцией).");
    // 2. access-control покрытие — только СКАЛЯРНЫЕ записи (привилегия), не map[msg.sender]
    for (const auto& ff : fns)
        if (!ff.internal && !ff.unmodeled && !ff.authGate && !ff.slotsWrittenScalar.empty())
            hyps.push_back("**Access control** — `" + ff.name + "` (public) пишет скалярный слот(ы) [" +
                joinInts(ff.slotsWrittenScalar) + "] без проверки msg.sender в теле. Намеренно? (SWC-105, Solodit SOL-AM-*).");
    // 3. oracle
    for (const auto& ff : fns)
        if (ff.oracleRead)
            hyps.push_back("**Price manipulation** — `" + ff.name + "` берёт spot-цену из DEX-резерва. "
                "Проверь на TWAP/Chainlink; флеш-займом цена искажается за 1 блок (SWC-oracle).");
    // 4. selfdestruct / delegatecall
    for (const auto& ff : fns) {
        if (ff.selfdestruct)
            hyps.push_back("**Selfdestruct** — `" + ff.name + "`: SWC-106, проверь права на вызов.");
        if (ff.callKinds.count("delegatecall"))
            hyps.push_back("**Delegatecall** — `" + ff.name + "`: чужой код в НАШЕМ storage (SWC-112). "
                "Проверь storage-collision и доверенность target-адреса.");
    }
    // 5. учётный инвариант / vault
    bool anyPayable = false, anyMap = false;
    for (const auto& ff : fns) { anyPayable |= ff.readsMsgValue; anyMap |= ff.usesMapElem; }
    if (anyPayable && anyMap)
        hyps.push_back("**Учётный инвариант** — контракт держит ether И per-user балансы (map[]). "
            "Инвариант: `Σ balances == address(this).balance`. Проверь first-depositor, share-inflation, "
            "направление округления convertToShares/assets (Solodit: ERC4626, Balancer-2025 rounding).");
    // 6. непрозрачный модификатор
    for (const auto& ff : fns)
        if (ff.unmodeled)
            hyps.push_back("**Непрозрачный модификатор** — `" + ff.name + "`: фронтенд не заинлайнил "
                "модификатор; гвард мог остаться в нём. Проверь исходник вручную, не доверяй авто-охвату.");
    // 7. tx.origin
    for (const auto& ff : fns)
        if (ff.readsTxOrigin)
            hyps.push_back("**tx.origin** — `" + ff.name + "`: авторизация по tx.origin ломается фишингом (SWC-115).");
    // 8. непрозрачная запись после вызова
    for (const auto& ff : fns)
        if (ff.opaqueStore)
            hyps.push_back("**Непрозрачный эффект** — `" + ff.name + "`: запись в struct/массив после внешнего "
                "вызова, точный слот неизвестен — возможная скрытая reentrancy, проверь вручную.");

    if (hyps.empty()) {
        os << "_Автоматических хуков не выделено — веди ручной обзор по чеклисту Solodit._\n\n";
    } else {
        for (const auto& h : hyps) os << "- " << h << "\n";
        os << "\n";
    }

    // ---- рекомендованные инварианты (форма → готовая forge-проверка) ----
    // замыкает петлю: детекция формы контракта → точная команда --emit-invariant, которую
    // аналитик сразу гоняет через verify_invariant.sh (forge доказывает/опровергает)
    {
        auto hasName = [&](const char* sub) {
            for (const auto& ff : fns) {
                std::string n = ff.name;
                std::transform(n.begin(), n.end(), n.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                if (n.find(sub) != std::string::npos) return true;
            }
            return false;
        };
        bool payable = false;
        for (const auto& ff : fns) if (ff.readsMsgValue) payable = true;
        const bool erc4626 = hasName("converttoshares") || hasName("converttoassets") ||
                             (hasName("deposit") && hasName("redeem") && hasName("totalassets"));
        const bool vaultEth = payable && (hasName("withdraw") || hasName("redeem"));
        const std::string src = meta.source.empty() ? "<src.sol>" : meta.source;

        os << "## Рекомендованные инварианты (форма → forge-доказательство)\n\n";
        if (!erc4626 && !vaultEth) {
            os << "_Явной vault/ERC4626-формы не распознано — инвариант пиши под гипотезу вручную "
                  "(`--emit-invariant` + правка свойства)._\n\n";
        } else {
            os << "SCSA распознал форму — гоняй готовые свойства (сломанный инвариант = "
                  "подтверждённый баг с контрпримером):\n\n";
            if (erc4626) {
                os << "- **ERC4626-форма** (convert*/totalAssets/deposit/redeem):\n";
                os << "  - округление: `benchmarks/verify_invariant.sh " << src << " roundtrip`\n";
                os << "  - first-depositor/inflation: `benchmarks/verify_invariant.sh " << src << " inflation-erc20`\n";
            }
            if (vaultEth) {
                os << "- **payable-vault форма** (msg.value + withdraw/redeem):\n";
                os << "  - solvency/no-overdraw: `benchmarks/verify_invariant.sh " << src << " solvency`\n";
                os << "  - ETH-inflation: `benchmarks/verify_invariant.sh " << src << " inflation`\n";
            }
            os << "  (для `--address` сохрани блок «Исходный код» ниже в .sol и подставь путь)\n\n";
        }
    }

    // ---- исходники ----
    os << "## Исходный код\n\n";
    for (const auto& [path, content] : files) {
        os << "### `" << path << "`\n\n```solidity\n" << content;
        if (!content.empty() && content.back() != '\n') os << "\n";
        os << "```\n\n";
    }
}

}  // namespace solc::report
