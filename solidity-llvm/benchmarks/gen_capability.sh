#!/usr/bin/env bash
# генератор + раннер МАТРИЦЫ ВОЗМОЖНОСТЕЙ: минимальный уязвимый кейс на каждый денежный класс.
# цель — честная карта «что SCSA детектит, а что нет» (баг-баунти охват). каждый файл несёт
# // expect: <класс> (или miss — заведомая дыра покрытия). раннер печатает HIT/MISS.
set -u
DIR="$(dirname "$0")/capability"
BIN="${BIN:-$(dirname "$0")/../build/src/solidity-llvm}"
mkdir -p "$DIR"

# --- ПОКРЫТО (санити-регресс) ---
cat > "$DIR/c01_reentrancy.sol" <<'SOL'
// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: reentrancy
contract C { mapping(address=>uint256) bal;
  function deposit() external payable { bal[msg.sender]+=msg.value; }
  function withdraw() external { uint256 a=bal[msg.sender]; (bool ok,)=msg.sender.call{value:a}(""); require(ok); bal[msg.sender]=0; } }
SOL

cat > "$DIR/c02_access_control.sol" <<'SOL'
// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: access-control
contract C { address owner; constructor(){owner=msg.sender;}
  function setOwner(address n) external { owner=n; }
  function guarded(address n) external { require(msg.sender==owner); owner=n; } }
SOL

cat > "$DIR/c03_delegatecall.sol" <<'SOL'
// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: delegatecall
contract C { function exec(address t, bytes calldata d) external { (bool ok,)=t.delegatecall(d); require(ok); } }
SOL

# --- ДЫРЫ ПОКРЫТИЯ (то, что баг-баунти платит, а мы, вероятно, пропускаем) ---

cat > "$DIR/g01_unprotected_init.sol" <<'SOL'
// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: access-control
// unprotected initializer (нет модификатора initializer): любой захватит owner (класс Nomad/прокси)
contract C { address owner;
  function initialize(address o) external { owner = o; }
  function adminSet(address o) external { require(msg.sender==owner); owner=o; } }
SOL

cat > "$DIR/g02_erc4626_inflation.sol" <<'SOL'
// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: miss
// first-deposit / share-inflation: supply==0 -> sh=assets, нет минимальных/dead-shares.
// атакующий mint 1 wei + донат -> инфляция цены доли -> следующий вкладчик округляется в 0
interface IERC20 { function transferFrom(address,address,uint256) external returns(bool); function balanceOf(address) external view returns(uint256); }
contract Vault { uint256 totalShares; mapping(address=>uint256) shares; IERC20 asset;
  function deposit(uint256 assets) external returns(uint256 sh) {
    uint256 supply = totalShares;
    sh = supply == 0 ? assets : assets * supply / asset.balanceOf(address(this));
    asset.transferFrom(msg.sender, address(this), assets);
    shares[msg.sender]+=sh; totalShares+=sh; } }
SOL

cat > "$DIR/g03_signature_replay.sol" <<'SOL'
// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: miss
// signature-replay: ecrecover авторизует выплату, но нет nonce/used[hash] -> повтор той же подписи
contract C { mapping(address=>uint256) bal;
  function claim(uint256 amount, uint8 v, bytes32 r, bytes32 s) external {
    bytes32 h = keccak256(abi.encodePacked(msg.sender, amount));
    address signer = ecrecover(h, v, r, s);
    require(signer != address(0));
    bal[msg.sender] += amount; } }
SOL

cat > "$DIR/g04_price_balanceof.sol" <<'SOL'
// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: miss
// price-manipulation через balanceOf-соотношение (манипулируемо донатом/flash-loan). мы намеренно
// исключили balanceOf из oracle_read (FP-склонно) -> класс не детектится
interface IERC20 { function balanceOf(address) external view returns(uint256); }
contract C { IERC20 tokenA; IERC20 tokenB;
  function priceBuy() external payable {
    uint256 price = tokenA.balanceOf(address(this)) / tokenB.balanceOf(address(this));
    (bool ok,)=msg.sender.call{value: price}(""); require(ok); } }
SOL

cat > "$DIR/g05_arbitrary_call.sol" <<'SOL'
// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: arbitrary-call
// arbitrary external call: target И calldata от юзера -> дренаж approvals контракта (Multichain/Socket).
// ПОКРЫТО ArbitraryCallDetector (2026-07-12): низкоуровневый .call(data), target из аргумента, без гарда
contract C { function execute(address target, bytes calldata data) external { (bool ok,)=target.call(data); require(ok); } }
SOL

cat > "$DIR/g06_fee_on_transfer.sol" <<'SOL'
// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: miss
// fee-on-transfer/accounting-delta: кредитуем amt, а не измеренную дельту balanceOf -> deflationary
// токен ломает учёт (кредит > реально полученного)
interface IERC20 { function transferFrom(address,address,uint256) external returns(bool); }
contract C { mapping(address=>uint256) bal; IERC20 token;
  function deposit(uint256 amt) external { token.transferFrom(msg.sender,address(this),amt); bal[msg.sender]+=amt; } }
SOL

cat > "$DIR/g07_unchecked_transfer.sol" <<'SOL'
// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
// expect: miss
// unchecked ERC20 transfer (не-реверт токен возвращает false): перевод «прошёл», но не случился.
// у нас это только LOW unchecked-call, не HIGH/MEDIUM
interface IERC20 { function transfer(address,uint256) external returns(bool); }
contract C { IERC20 token; function pay(address to, uint256 a) external { token.transfer(to, a); } }
SOL

echo "сгенерировано $(ls "$DIR"/*.sol | wc -l) проб. Прогон:"
printf "%-28s %-16s %-22s %s\n" "проба" "ожидалось" "детектнуто(H/M)" "итог"
for f in "$DIR"/*.sol; do
  expect=$(grep -oE 'expect: *[a-z-]+' "$f" | head -1 | sed 's/expect: *//')
  out=$("$BIN" --input "$f" --audit 2>/dev/null || true)
  got=""
  for rule in read-only-reentrancy reentrancy access-control arbitrary-call weak-randomness price-manipulation selfdestruct delegatecall tx-origin; do
    echo "$out" | grep -qE "\[(HIGH|MEDIUM)\] $rule" && got="$got $rule"
  done
  got=$(echo "$got" | xargs); [ -z "$got" ] && got="none"
  if [ "$expect" = "miss" ]; then
    [ "$got" = "none" ] && verdict="GAP(ожид)" || verdict="ПОЙМАЛ!($got)"
  else
    echo "$got" | grep -qw "$expect" && verdict="HIT" || verdict="ПРОПУСК!"
  fi
  printf "%-28s %-16s %-22s %s\n" "$(basename "$f")" "$expect" "$got" "$verdict"
done
