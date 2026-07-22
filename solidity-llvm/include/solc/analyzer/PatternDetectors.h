#pragma once
#include <llvm/IR/Module.h>

#include "solc/report/Finding.h"

namespace solc::analyzer {

// tx.origin в проверке прав (SWC-115): фишинг-уязвимость. tx.origin — исходный
// внешний отправитель транзакции; сравнивать его для авторизации нельзя, т.к.
// жертву можно заманить вызвать вредоносный контракт-посредник.
class TxOriginDetector {
public:
    report::Findings analyze(const llvm::Module& m);
};

// непроверенный результат внешнего вызова (SWC-104): low-level call возвращает
// флаг успеха; если его игнорируют, сбой вызова остаётся незамеченным.
class UncheckedCallDetector {
public:
    report::Findings analyze(const llvm::Module& m);
};

// неограниченный selfdestruct (SWC-106): функция уничтожает контракт без проверки
// прав — любой может её вызвать (ср. Parity wallet).
class SelfdestructDetector {
public:
    report::Findings analyze(const llvm::Module& m);
};

// delegatecall в недоверенный target (SWC-112): исполнение чужого кода в контексте
// нашего storage; если адрес не константный (msg.sender/параметр/storage) — опасно.
class DelegatecallDetector {
public:
    report::Findings analyze(const llvm::Module& m);
};

// слабый источник случайности (SWC-120): значение блока (block.timestamp/number)
// используется в операции выбора (% / /) — «случайность», предсказуемая/влияемая
// майнером. типовой баг лотерей/минтов/игр (winner = block.timestamp % players).
class WeakRandomnessDetector {
public:
    report::Findings analyze(const llvm::Module& m);
};

// spot-price манипуляция (flash-loan): значение из DEX-spot-источника (getReserves/
// slot0/getAmountsOut — маркер @scsa.oracle_read) форвард-тейнтом доходит до отправки
// средств или записи баланса без TWAP/guard. атакующий флеш-займом искажает пул →
// контракт читает манипулированную цену → профит. Slither плохо ловит → мало дубликатов.
class OraclePriceDetector {
public:
    report::Findings analyze(const llvm::Module& m);
};

// Chainlink-цена без проверки свежести (Yarus-2, частый класс — LoopFi «deprecated
// answeredInRound», Peapods «circuit breaker minAnswer/maxAnswer»): функция читает
// AggregatorV3Interface (latestRoundData/latestAnswer — маркер @scsa.oracle_read с меткой
// scsa.oracle.chainlink), но НЕ обращается к block.timestamp → updatedAt не сверяется →
// устаревшая цена принимается как валидная. эвристика по наличию block.timestamp в функции.
class OracleStalenessDetector {
public:
    report::Findings analyze(const llvm::Module& m);
};

// arbitrary external call (доминирующий класс bounty-хаков 2023-24: Multichain $126M,
// Socket, LiFi, Dexible, Seneca): контракт делает `target.call(data)`, где target приходит
// из аргументов функции (attacker-controlled), а точка входа НЕ защищена контролем прав.
// атакующий заставляет контракт вызвать что угодно от своего имени → дренаж одобренных
// (approve) контракту токенов через token.transferFrom(жертва, атакующий). delegatecall
// покрыт отдельно (DelegatecallDetector); здесь обычный CALL с недоверенным target.
class ArbitraryCallDetector {
public:
    report::Findings analyze(const llvm::Module& m);
};

// отсутствующая slippage-защита на DEX-свопе (Yarus-2, частый — LoopFi «no slippage minOut»,
// Peapods): функция вызывает router-своп (`swapExactTokensForTokens`/`exactInputSingle`/… —
// маркер `scsa.swap`), но НЕ содержит сравнения против параметра из calldata (require(out>=minOut)).
// эвристика (выход свопа IR не моделирует) → MEDIUM-кандидат для верификации аналитиком.
class SlippageDetector {
public:
    report::Findings analyze(const llvm::Module& m);
};

}  // namespace solc::analyzer
