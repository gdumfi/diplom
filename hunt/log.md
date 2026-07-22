# HUNT LOG — журнал итераций цикла (новое снизу дописывает цикл)

| время | цель | chain | исход | причина/детали |
|-------|------|-------|-------|----------------|
| 2026-07-21 14:37 | — | — | НЕТ ЦЕЛЕЙ | seed пуст, queue пуста, фидер недоступен (нет RPC_URL/ETHERSCAN_API_KEY); жду seed |
| 2026-07-21 17:24 | Tare (sherlock $50k) | — | CLEAN (4 FP) | 4 AC-кандидата — ВСЕ FP: updateNav/requestDeposit/cancelDepositRequest/createOffer защищены OZ hasRole()/_requireInvestor/isRegisteredForRole (детектор не видит вложенный role-mapping — ROADMAP #6) + accounting-счётчики. 0 сабмиттабельных |
