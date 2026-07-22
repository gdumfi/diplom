#pragma once
#include <string>

#include "solc/report/Finding.h"
#include "solc/report/PocGenerator.h"   // report::ForkConfig (форк-режим против живой жертвы)

namespace solc::report {

// генератор Foundry stateful-invariant харнесса (Фаза B нейро-символической схемы).
// в отличие от PoC (доказывает ОДИН конкретный путь), инвариант-харнесс даёт фаззеру
// (forge/medusa) ломать СВОЙСТВО перебором последовательностей вызовов — ловит классы,
// невидимые паттерн-детекторам: битый учёт, округление, share-inflation, а также
// reentrancy (handler c реентрантным receive). это инструмент, которым Claude-аналитик
// ДОКАЗЫВАЕТ гипотезу из контекст-пака, а не только читает факты.
class InvariantGen {
public:
    // харнесс «no-overdraw / solvency» для vault-формы (payable-депозит + вывод).
    // Handler = атакующий-актор: фаззит deposit/withdraw, реентрантный receive повторяет
    // вывод (ограниченная глубина). инвариант invariant_noOverdraw: withdrawn <= deposited
    // (атакующий не может забрать больше внесённого — т.е. не крадёт засеянный чужой TVL).
    // f даёт funding-функцию (депозит) и vuln-функцию (вывод) + типы их параметров.
    std::string generateSolvency(const std::string& contract, const Finding& f);

    // харнесс «first-depositor / share-inflation» — highest-paying механически-детектируемый
    // класс (Balancer/ERC4626 2025). Handler: атакующий вносит крошечно, донатит (раздувает
    // цену доли), жертва вносит и округляется в 0 долей, атакующий выкупает всё. инвариант
    // invariant_noInflationProfit: withdrawn <= deposited + donated (не украл депозит жертвы).
    // допущение формы: deposit() payable returns(uint256) + redeem(uint256) returns(uint256).
    // для ERC20-ERC4626 (asset/approve) интерфейс адаптирует аналитик — честная граница.
    std::string generateInflation(const std::string& contract);

    // то же first-depositor/share-inflation, но для РЕАЛЬНОЙ формы ERC4626 над ERC20-активом:
    // харнесс включает inline MockERC20 (mint/approve/transferFrom), деплоит vault(asset),
    // донат = asset.transfer в пул. допущение формы: constructor(address asset),
    // deposit(uint256 assets) returns(uint256 shares), redeem(uint256 shares) returns(uint256).
    std::string generateInflationErc20(const std::string& contract);

    // то же, но под СТАНДАРТНЫЙ ERC4626-интерфейс (реальные vault-ы: OZ-based, TermMax StableERC4626):
    // deposit(uint256 assets, address receiver) returns(shares), redeem(uint256 shares, address
    // receiver, address owner) returns(assets). допущение формы: constructor(address asset).
    // снимает границу «упрощённая форма» — харнесс применим к боевым ERC4626 из scope баунти.
    std::string generateInflationErc4626(const std::string& contract);

    // харнесс round-trip округления: convertToAssets(convertToShares(x)) <= x должно держаться
    // ВСЕГДА (нельзя из x активов сделать долей и обратно больше x). setUp сеет раздутый курс
    // (1 доля + донат), Handler фаззит x и флагует нарушение. ловит rounding-creates-value
    // (класс Balancer-2025). допущение формы: constructor(address asset), deposit(uint256),
    // convertToShares(uint256) view, convertToAssets(uint256) view.
    // fork.enabled → ФОРК-режим: харнесс не деплоит, а бьётся о РЕАЛЬНЫЙ ERC4626 по адресу
    // (vm.createSelectFork). view-only, работает при ЛЮБОМ состоянии пула — оружие для охоты
    // по адресу из scope баунти-программы. интерфейс ERC4626 (convertToShares/Assets).
    std::string generateRoundtrip(const std::string& contract, const ForkConfig& fork = {});

    // харнесс supply-integrity: не-владелец НЕ может увеличить свой баланс/эмиссию. атакующий
    // (handler) фаззит публичную поверхность; инвариант balanceOf(attacker)==0 (он ничего не
    // получал легитимно). ломается на «anyone can mint» / битом контроле эмиссии (частый хак).
    // допущение формы: mint(address,uint256), balanceOf(address) view.
    std::string generateSupply(const std::string& contract);
};

}  // namespace solc::report
