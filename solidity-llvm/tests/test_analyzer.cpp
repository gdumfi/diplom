#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <sstream>

#include "solc/analyzer/ReentrancyDetector.h"
#include "solc/analyzer/AnalysisPrep.h"
#include "solc/codegen/CodeGen.h"
#include "solc/lexer/Lexer.h"
#include "solc/parser/Parser.h"
#include "solc/sema/Sema.h"

// CodeGen владеет LLVMContext — держим его живым, пока анализируем модуль
struct Built {
    std::unique_ptr<solc::codegen::CodeGen> cg;
    std::unique_ptr<llvm::Module>           mod;
    operator bool() const { return mod != nullptr; }
};

static Built build(const char* src) {
    solc::DiagnosticEngine d;
    solc::Lexer lex(src, d);
    solc::Parser p(lex, d);
    auto c = p.parseContract();
    if (!c) return {};
    solc::sema::SemanticAnalyzer sa(d);
    if (!sa.analyze(*c)) return {};
    auto cg = std::make_unique<solc::codegen::CodeGen>(d);
    auto mod = cg->generate(*c);
    if (mod) solc::analyzer::promoteLocals(*mod);
    return {std::move(cg), std::move(mod)};
}

static std::string slurp(const std::string& rel) {
    std::ifstream f(std::string(SOLC_TESTS_DIR) + "/" + rel);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

TEST(Analyzer, FlagsReentrancyInFixture) {
    auto src = slurp("fixtures/reentrancy.sol");
    ASSERT_FALSE(src.empty());
    auto b = build(src.c_str());
    ASSERT_TRUE((bool)b);

    solc::analyzer::ReentrancyDetector det;
    auto findings = det.analyze(*b.mod);

    ASSERT_EQ(findings.size(), 1u);
    EXPECT_EQ(findings[0].rule, "reentrancy");
    EXPECT_EQ(findings[0].severity, "HIGH");
    EXPECT_EQ(findings[0].function, "withdraw");
    EXPECT_EQ(findings[0].storageSlot, 0);
    // строка эффекта берётся с !dbg storage-записи — должна быть валидной (>0)
    EXPECT_GT(findings[0].line, 0);
}

// функция под nonReentrant-guard (require(!locked); locked=true; ...call...):
// порядок «вызов -> запись storage» тот же, что в reentrancy.sol, но guard
// распознан — HIGH быть НЕ должно (допускается пусто либо LOW с пометкой guarded)
TEST(Analyzer, GuardedContractNotHigh) {
    auto src = slurp("fixtures/reentrancy_guarded.sol");
    ASSERT_FALSE(src.empty());
    auto b = build(src.c_str());
    ASSERT_TRUE((bool)b);

    solc::analyzer::ReentrancyDetector det;
    auto findings = det.analyze(*b.mod);

    // ни одной HIGH-находки; всё, что осталось, — только LOW (guarded)
    for (const auto& f : findings) {
        EXPECT_NE(f.severity, "HIGH")
            << "guard должен подавлять/понижать находку, а не оставлять HIGH";
        EXPECT_EQ(f.severity, "LOW");
    }
    // release-флаг (locked = false, слот 0) не должен порождать находку —
    // остаётся ровно одна LOW на реальную запись эффекта balance (слот 1)
    ASSERT_EQ(findings.size(), 1u);
    EXPECT_EQ(findings[0].severity, "LOW");
    EXPECT_EQ(findings[0].function, "withdraw");
    EXPECT_EQ(findings[0].storageSlot, 1);
    EXPECT_GT(findings[0].line, 0);
}

// внешний вызов ПОСЛЕ обновления storage — checks-effects-interactions соблюдён,
// находок быть не должно
TEST(Analyzer, NoFindingWhenEffectBeforeInteraction) {
    auto b = build(
        "contract Safe {"
        "  uint256 balance;"
        "  function withdraw() public {"
        "    require(balance > 0);"
        "    balance = 0;"
        "    msg.sender.call{value: balance}(\"\");"
        "  }"
        "}");
    ASSERT_TRUE((bool)b);
    solc::analyzer::ReentrancyDetector det;
    EXPECT_TRUE(det.analyze(*b.mod).empty());
}

// нет внешних вызовов вообще — нечего находить
TEST(Analyzer, NoFindingWithoutExternalCall) {
    auto src = slurp("fixtures/counter.sol");
    ASSERT_FALSE(src.empty());
    auto b = build(src.c_str());
    ASSERT_TRUE((bool)b);
    solc::analyzer::ReentrancyDetector det;
    EXPECT_TRUE(det.analyze(*b.mod).empty());
}

// внешний вызов есть, но записи в storage после него нет — не reentrancy
TEST(Analyzer, NoFindingWhenNoStoreAfterCall) {
    auto b = build(
        "contract C {"
        "  function ping() public { msg.sender.call{value: 1}(\"\"); }"
        "}");
    ASSERT_TRUE((bool)b);
    solc::analyzer::ReentrancyDetector det;
    EXPECT_TRUE(det.analyze(*b.mod).empty());
}

// контрпример из адверсариального ревью: require(active) — НЕ nonReentrant lock
// (нет негации !, и флаг ставится ДО проверки). реальная запись balance после
// внешнего вызова обязана остаться HIGH, а не быть понижена до LOW [guarded].
TEST(Analyzer, BusinessFlagNotMistakenForGuard) {
    auto b = build(
        "contract Bank {"
        "  bool active;"
        "  uint256 balance;"
        "  function withdraw() public {"
        "    active = true;"
        "    require(active);"
        "    msg.sender.call{value: balance}(\"\");"
        "    balance = 0;"
        "  }"
        "}");
    ASSERT_TRUE((bool)b);
    solc::analyzer::ReentrancyDetector det;
    auto fs = det.analyze(*b.mod);
    ASSERT_EQ(fs.size(), 1u);
    EXPECT_EQ(fs[0].severity, "HIGH");
    EXPECT_EQ(fs[0].storageSlot, 1);
}

// guard снят до вызова (locked=true; locked=false; call) — на момент вызова
// защиты нет: должно остаться HIGH, а не быть понижено до guarded/LOW
TEST(Analyzer, GuardReleasedBeforeCallStaysHigh) {
    auto b = build(
        "contract V {"
        "  bool locked;"
        "  uint256 balance;"
        "  function withdraw() public {"
        "    require(!locked);"
        "    locked = true;"
        "    locked = false;"
        "    msg.sender.call{value: balance}(\"\");"
        "    balance = 0;"
        "  }"
        "}");
    ASSERT_TRUE((bool)b);
    solc::analyzer::ReentrancyDetector det;
    auto fs = det.analyze(*b.mod);
    ASSERT_EQ(fs.size(), 1u);
    EXPECT_EQ(fs[0].severity, "HIGH");
    EXPECT_EQ(fs[0].storageSlot, 1);
}

// per-user reentrancy на mapping (balances[msg.sender] = 0 после вызова) → HIGH
TEST(Analyzer, MappingReentrancyFlagged) {
    auto src = slurp("fixtures/reentrancy_mapping.sol");
    ASSERT_FALSE(src.empty());
    auto b = build(src.c_str());
    ASSERT_TRUE((bool)b);
    solc::analyzer::ReentrancyDetector det;
    auto fs = det.analyze(*b.mod);
    ASSERT_EQ(fs.size(), 1u);
    EXPECT_EQ(fs[0].severity, "HIGH");
    EXPECT_EQ(fs[0].function, "withdraw");
    EXPECT_GT(fs[0].line, 0);
}

// mapping с CEI-корректным порядком (обнуление до вызова) → чисто
TEST(Analyzer, MappingCeiSafe) {
    auto b = build(
        "contract V {"
        "  mapping(address => uint256) balances;"
        "  function withdraw(uint256 amt) public {"
        "    require(balances[msg.sender] > 0);"
        "    balances[msg.sender] = 0;"
        "    msg.sender.call{value: amt}(\"\");"
        "  }"
        "}");
    ASSERT_TRUE((bool)b);
    solc::analyzer::ReentrancyDetector det;
    EXPECT_TRUE(det.analyze(*b.mod).empty());
}

// read-only reentrancy с триггером .call{value:0} (НЕ ETH-value): раньше пропускался
// (детектор требовал value>0), теперь ловится — canReenter расширен на недоверенные
// не-static вызовы (токен-хуки/произвольный callback). view-геттер price() читает total,
// мутируемый ПОСЛЕ вызова в sync → устаревшее чтение во время callback
TEST(Analyzer, ReadOnlyReentrancyCallTrigger) {
    auto b = build(
        "contract Pool {"
        "  uint256 total;"
        "  function sync(address cb) public {"
        "    cb.call{value: 0}(\"\");"
        "    total = total + 1;"
        "  }"
        "  function price() public view returns (uint256) { return total; }"
        "}");
    ASSERT_TRUE((bool)b);
    solc::analyzer::ReentrancyDetector det;
    auto fs = det.analyze(*b.mod);
    bool ro = false;
    for (const auto& f : fs)
        if (f.rule == "read-only-reentrancy" && f.function == "price") ro = true;
    EXPECT_TRUE(ro);
}

// боевой Solidity 0.4.x синтаксис: pragma, uint-алиас, visibility опущена,
// compound -=, старый low-level call msg.sender.call.value(x)(). должно ловиться HIGH
TEST(Analyzer, LegacySoliditySyntaxReentrancy) {
    auto b = build(
        "pragma solidity ^0.4.2;"
        "contract SimpleDAO {"
        "  mapping(address => uint) credit;"
        "  function withdraw(uint amount) {"
        "    if (credit[msg.sender] >= amount) {"
        "      bool res = msg.sender.call.value(amount)();"
        "      credit[msg.sender] -= amount;"
        "    }"
        "  }"
        "}");
    ASSERT_TRUE((bool)b);
    solc::analyzer::ReentrancyDetector det;
    auto fs = det.analyze(*b.mod);
    ASSERT_EQ(fs.size(), 1u);
    EXPECT_EQ(fs[0].severity, "HIGH");
    EXPECT_EQ(fs[0].function, "withdraw");
}

// nonReentrant как МОДИФИКАТОР: guard инлайнится в тело -> детектор его учитывает,
// reentrancy НЕ должна быть HIGH (guard распознан на инлайненном IR)
TEST(Analyzer, NonReentrantModifierRecognized) {
    auto b = build(
        "contract Vault {"
        "  bool locked;"
        "  uint256 balance;"
        "  modifier nonReentrant() { require(!locked); locked = true; _; locked = false; }"
        "  function withdraw() public nonReentrant {"
        "    msg.sender.call{value: balance}(\"\");"
        "    balance = 0;"
        "  }"
        "}");
    ASSERT_TRUE((bool)b);
    solc::analyzer::ReentrancyDetector det;
    auto fs = det.analyze(*b.mod);
    for (const auto& f : fs) EXPECT_NE(f.severity, "HIGH")
        << "инлайненный nonReentrant-guard должен подавлять HIGH";
}

// регресс (ревью): модификатор БЕЗ _; — тело функции не должно пропадать из IR,
// reentrancy обязана обнаружиться (иначе ложный негатив)
TEST(Analyzer, ModifierWithoutPlaceholderKeepsBody) {
    auto b = build(
        "contract V { bool g; uint256 balance;"
        "  modifier chk() { require(!g); }"
        "  function withdraw() public chk {"
        "    msg.sender.call{value: balance}(\"\");"
        "    balance = 0;"
        "  } }");
    ASSERT_TRUE((bool)b);
    solc::analyzer::ReentrancyDetector det;
    auto fs = det.analyze(*b.mod);
    ASSERT_EQ(fs.size(), 1u);
    EXPECT_EQ(fs[0].severity, "HIGH");
}

// регресс (ревью): модификатор С ПАРАМЕТРОМ не должен ронять codegen
TEST(Analyzer, ModifierWithParamDoesNotCrash) {
    auto b = build(
        "contract R { uint256 config;"
        "  modifier onlyRole(bool r) { require(r); _; }"
        "  function setConfig(uint256 c) public onlyRole(true) { config = c; } }");
    ASSERT_TRUE((bool)b);   // главное — скомпилировалось без 'local not found'
}

// регресс (ревью, DominatorTree): guard, ВЗВОДИМЫЙ в ветке if, не доминирует вызов,
// значит защита не гарантирована на всех путях -> должно остаться HIGH, а не LOW
TEST(Analyzer, ConditionalGuardNotSuppressed) {
    auto b = build(
        "contract V { bool locked; uint256 balance; uint256 cond;"
        "  function withdraw() public {"
        "    require(!locked);"
        "    if (cond > 0) { locked = true; }"
        "    msg.sender.call{value: balance}(\"\");"
        "    balance = 0;"
        "  } }");
    ASSERT_TRUE((bool)b);
    solc::analyzer::ReentrancyDetector det;
    auto fs = det.analyze(*b.mod);
    ASSERT_EQ(fs.size(), 1u);
    EXPECT_EQ(fs[0].severity, "HIGH");
}

// регресс (ревью): nonReentrant-guard внутри require(!locked && cond) распознаётся
// (codegen && = and i1) — иначе ложный HIGH на защищённом контракте
TEST(Analyzer, GuardInsideAndRecognized) {
    auto b = build(
        "contract V { bool locked; uint256 balance;"
        "  function withdraw(uint256 amt) public {"
        "    require(!locked && amt > 0); locked = true;"
        "    msg.sender.call{value: amt}(\"\"); balance = 0; locked = false; } }");
    ASSERT_TRUE((bool)b);
    solc::analyzer::ReentrancyDetector det;
    auto fs = det.analyze(*b.mod);
    for (const auto& f : fs) EXPECT_NE(f.severity, "HIGH");
}

// регресс (пункт 1): event/emit/require-message/if-без-скобок/revert() компилируются,
// reentrancy по-прежнему ловится
TEST(Analyzer, StructuralBatchFeaturesCompileAndDetect) {
    auto b = build(
        "contract Vault {"
        "  mapping(address => uint256) balances;"
        "  event W(address who);"
        "  function withdraw() public {"
        "    require(balances[msg.sender] > 0, \"no balance\");"
        "    if (balances[msg.sender] == 0) revert();"
        "    emit W(msg.sender);"
        "    msg.sender.call{value: balances[msg.sender]}(\"\");"
        "    balances[msg.sender] = 0;"
        "  } }");
    ASSERT_TRUE((bool)b);
    solc::analyzer::ReentrancyDetector det;
    auto fs = det.analyze(*b.mod);
    ASSERT_EQ(fs.size(), 1u);
    EXPECT_EQ(fs[0].severity, "HIGH");
}

// регресс (ревью, пункт 4): cross-function reentrancy — payout читает share и шлёт
// value, но обнуляет share функция markPaid; должно дать MEDIUM (был пропуск)
TEST(Analyzer, CrossFunctionReentrancyFlagged) {
    auto b = build(
        "contract Payouts {"
        "  mapping(address => uint256) share;"
        "  function payout() public {"
        "    require(share[msg.sender] > 0);"
        "    msg.sender.call{value: share[msg.sender]}(\"\");"
        "  }"
        "  function markPaid(address who) public { share[who] = 0; } }");
    ASSERT_TRUE((bool)b);
    solc::analyzer::ReentrancyDetector det;
    auto fs = det.analyze(*b.mod);
    bool crossFn = false;
    for (const auto& f : fs)
        if (f.rule == "reentrancy" && f.severity == "MEDIUM" && f.function == "payout") crossFn = true;
    EXPECT_TRUE(crossFn);
}

// регресс (П7): reentrancy через ВНУТРЕННИЙ вызов — G вызывает H (делает внешний
// вызов), затем пишет storage. должно быть HIGH (межпроцедурно)
TEST(Analyzer, InternalCallReentrancy) {
    auto b = build(
        "contract Bonus {"
        "  mapping(address => bool) claimed;"
        "  mapping(address => uint256) rewards;"
        "  function withdraw(address r) public {"
        "    uint256 a = rewards[r]; rewards[r] = 0;"
        "    r.call{value: a}(\"\");"
        "  }"
        "  function claim(address r) public {"
        "    require(!claimed[r]); rewards[r] = 100;"
        "    withdraw(r);"           // внутренний вызов функции с внешним вызовом
        "    claimed[r] = true;"     // эффект ПОСЛЕ -> reentrancy
        "  } }");
    ASSERT_TRUE((bool)b);
    solc::analyzer::ReentrancyDetector det;
    bool high = false;
    for (const auto& f : det.analyze(*b.mod))
        if (f.severity == "HIGH" && f.function == "claim") high = true;
    EXPECT_TRUE(high);
}

// регресс (Этап 4.5, storage-ссылка): honeypot PrivateBank — `var acc = Acc[msg.sender]`
// это storage-ссылка старого Solidity, поэтому `acc.balance -= _am` ПОСЛЕ внешнего вызова
// пишет в storage → reentrancy. без моделирования алиаса был бы ложный негатив
TEST(Analyzer, VarStorageAliasReentrancy) {
    auto b = build(
        "contract PrivateBank {"
        "  mapping(address => Holder) Acc;"
        "  uint256 MinSum;"
        "  function Collect(uint256 _am) public {"
        "    var acc = Acc[msg.sender];"
        "    if (acc.balance >= MinSum) {"
        "      if (msg.sender.call{value: _am}(\"\")) {"
        "        acc.balance -= _am;"
        "      }"
        "    }"
        "  } }");
    ASSERT_TRUE((bool)b);
    solc::analyzer::ReentrancyDetector det;
    bool high = false;
    for (const auto& f : det.analyze(*b.mod))
        if (f.severity == "HIGH" && f.function == "Collect") high = true;
    EXPECT_TRUE(high);
}

// парный негатив: та же storage-ссылка, но эффект ДО вызова (CEI) — не должно быть HIGH.
// замок precision на alias-пути: алиас-запись не создаёт ложных срабатываний
TEST(Analyzer, VarStorageAliasSafeCEI) {
    auto b = build(
        "contract PrivateBank {"
        "  mapping(address => Holder) Acc;"
        "  function Collect(uint256 _am) public {"
        "    var acc = Acc[msg.sender];"
        "    acc.balance -= _am;"
        "    msg.sender.call{value: _am}(\"\");"
        "  } }");
    ASSERT_TRUE((bool)b);
    solc::analyzer::ReentrancyDetector det;
    bool high = false;
    for (const auto& f : det.analyze(*b.mod))
        if (f.severity == "HIGH") high = true;
    EXPECT_FALSE(high);
}

// регресс (П7): агрегаты (массив, поле struct, высокоуровневый метод-вызов)
// компилируются как opaque, low-level reentrancy при этом ловится
TEST(Analyzer, OpaqueAggregatesCompile) {
    auto b = build(
        "contract Honey {"
        "  mapping(address => uint256) balances;"
        "  address Log;"
        "  address[] History;"
        "  function collect(uint256 amt) public {"
        "    History[0] = msg.sender;"                 // индексация массива (opaque)
        "    msg.sender.call{value: amt}(\"\");"
        "    balances[msg.sender] = 0;"                 // эффект после вызова -> HIGH
        "  } }");
    ASSERT_TRUE((bool)b);
    solc::analyzer::ReentrancyDetector det;
    auto fs = det.analyze(*b.mod);
    bool high = false;
    for (const auto& f : fs) if (f.severity == "HIGH") high = true;
    EXPECT_TRUE(high);
}

// непрозрачная запись в storage-агрегат (поле struct) ПОСЛЕ вызова → LOW-предупреждение
// (честная граница: раньше молча пропускалось = ложный негатив)
TEST(Analyzer, OpaqueStoreAfterCallWarnsLow) {
    auto b = build(
        "contract S { uint256 bal; Data data;"
        "  function withdraw() public {"
        "    require(bal > 0);"
        "    msg.sender.call{value: bal}(\"\");"
        "    data.balance = 0; } }");   // непрозрачная запись поля struct после вызова
    ASSERT_TRUE((bool)b);
    solc::analyzer::ReentrancyDetector det;
    bool low = false;
    for (const auto& f : det.analyze(*b.mod))
        if (f.rule == "reentrancy" && f.severity == "LOW" &&
            f.message.find("непрозрачная") != std::string::npos) low = true;
    EXPECT_TRUE(low);
}

// та же непрозрачная запись, но ДО вызова (CEI) — не предупреждаем
TEST(Analyzer, OpaqueStoreBeforeCallClean) {
    auto b = build(
        "contract S { uint256 bal; Data data;"
        "  function withdraw() public {"
        "    data.balance = 0;"
        "    msg.sender.call{value: bal}(\"\"); } }");
    ASSERT_TRUE((bool)b);
    solc::analyzer::ReentrancyDetector det;
    bool any = false;
    for (const auto& f : det.analyze(*b.mod))
        if (f.rule == "reentrancy") any = true;
    EXPECT_FALSE(any);
}
