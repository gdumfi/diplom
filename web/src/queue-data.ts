// Batch-funnel rows — mirrors `hunt_batch.sh` output (deterministic sca --audit pass,
// bucketed by the SCSA_SKIP reason codes). Targets/results reflect real runs:
// Seaport/Permit2/1inch lower fine; 0.4 honeypots skip as pre-0.8 (bucket C).

export type QState = "survivor" | "minor" | "clean" | "skip"

export interface QRow {
  target: string
  state: QState
  detail: string
  code?: string // reason code for skips (A0/A1/A2/A3/B1/C)
}

// reason codes, straight from Driver::emitSkip / ROBUSTNESS-PLAN §2
export const SKIP_CODES: Record<string, string> = {
  A0: "fetch failed (Etherscan)",
  A1: "no solc image",
  A2: "wrong solc version",
  A3: "solc compile error",
  B1: "lowering failed",
  C: "pre-0.8 (out of scope)",
}

export const QUEUE: QRow[] = [
  { target: "SampleVault.sol", state: "survivor", detail: "4 HIGH · access-control · arbitrary-call · reentrancy" },
  { target: "0x2f9a…LenderPool", state: "survivor", detail: "1 HIGH · share-inflation" },
  { target: "0x1111…AggregationRouter V5", state: "minor", detail: "1 MED · 22 LOW" },
  { target: "0x0000…Permit2", state: "minor", detail: "2 LOW" },
  { target: "0x0000…Seaport 1.5", state: "clean", detail: "no findings — lowered OK" },
  { target: "0x0000…Seaport 1.1", state: "clean", detail: "no findings — lowered OK" },
  { target: "0x6b17…DAI", state: "skip", code: "C", detail: "pre-0.8 (0.5.12)" },
  { target: "0x01f8…PrivateBank", state: "skip", code: "C", detail: "pre-0.8 (0.4.19)" },
  { target: "0x23a9…honeypot", state: "skip", code: "C", detail: "pre-0.8 (0.4.19)" },
  { target: "0x0486…MultiplicatorX3", state: "skip", code: "C", detail: "pre-0.8 (0.4.24)" },
  { target: "0x8f3c…OldVault", state: "skip", code: "A3", detail: "solc: TypeError — data location" },
  { target: "0xd0b5…proxy", state: "skip", code: "A0", detail: "fetch failed — not verified" },
]
