import { useState } from "react"
import { motion } from "motion/react"
import { Play, Loader2, FlaskConical, TriangleAlert, FileCode2 } from "lucide-react"
import { Button } from "@/components/ui/button"
import { Input } from "@/components/ui/input"
import { Card } from "@/components/ui/card"
import { cn } from "@/lib/utils"
import { rise, stagger } from "@/lib/motion"
import { genInvariant, type InvResult } from "@/lib/api"

const PROPS: Record<string, { label: string; blurb: string }> = {
  solvency: { label: "solvency", blurb: "no-overdraw — withdrawn ≤ deposited. Breaks on reentrancy or broken accounting." },
  inflation: { label: "inflation", blurb: "first-depositor share-inflation on an ETH vault — withdrawn ≤ deposited + donated." },
  "inflation-erc20": { label: "inflation-erc20", blurb: "first-depositor for a real ERC4626 over an ERC20 asset (mock asset, approvals wired)." },
  roundtrip: { label: "roundtrip", blurb: "rounding creates value — convertToAssets(convertToShares(x)) ≤ x must hold." },
}

// встроенный образец — если цель не задана, генерим харнесс по нему (всегда работает)
const SAMPLE = `// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;
contract Vault {
    mapping(address => uint256) public bal;
    function deposit() external payable { bal[msg.sender] += msg.value; }
    function withdraw() external {
        uint256 a = bal[msg.sender];
        (bool ok,) = msg.sender.call{value: a}("");
        require(ok);
        bal[msg.sender] = 0;
    }
}`

export default function Invariants() {
  const [prop, setProp] = useState("solvency")
  const [target, setTarget] = useState("")
  const [chain, setChain] = useState("1")
  const [res, setRes] = useState<InvResult | null>(null)
  const [busy, setBusy] = useState(false)
  const [err, setErr] = useState("")

  async function run() {
    setErr(""); setRes(null); setBusy(true)
    try {
      const t = target.trim()
      const req = t
        ? { mode: "address" as const, address: t, chain: Number(chain) || 1, property: prop }
        : { mode: "source" as const, source: SAMPLE, property: prop }
      if (req.mode === "address" && !/^0x[0-9a-fA-F]{40}$/.test(t)) {
        setErr("Адрес должен быть 0x… (или оставь пустым — возьму встроенный образец)."); setBusy(false); return
      }
      setRes(await genInvariant(req))
    } catch (e) {
      setErr(e instanceof Error ? e.message : "invariant failed")
    } finally {
      setBusy(false)
    }
  }

  return (
    <motion.div variants={stagger} initial="hidden" animate="show" className="mx-auto max-w-5xl">
      <motion.div variants={rise}>
        <p className="font-mono text-xs font-semibold uppercase tracking-[0.14em] text-primary">Invariant runner</p>
        <h1 className="mt-1 text-2xl font-bold tracking-tight">Scaffold a money-class invariant</h1>
        <p className="mt-1.5 max-w-[62ch] text-sm text-muted-foreground">
          Pick a property and a target — SCSA generates a real Foundry invariant handler.
          Run it with <span className="font-mono text-foreground">forge test</span>; a failing invariant is a confirmed exploit.
        </p>
      </motion.div>

      {/* target */}
      <motion.div variants={rise}>
        <Card className="mt-5 gap-3 p-4">
          <div className="grid gap-3 sm:grid-cols-[1fr_110px]">
            <Input value={target} onChange={(e) => setTarget(e.target.value)}
              placeholder="0x… адрес цели  (пусто = встроенный образец)" className="font-mono" />
            <Input value={chain} onChange={(e) => setChain(e.target.value)} placeholder="chain" className="font-mono" />
          </div>
          {err && (
            <div className="flex items-start gap-2 rounded-lg border border-destructive/40 bg-destructive/10 px-3 py-2 text-sm text-destructive">
              <TriangleAlert className="mt-0.5 size-4 shrink-0" /> <span>{err}</span>
            </div>
          )}
        </Card>
      </motion.div>

      <motion.div variants={rise} className="mt-4 grid gap-4 lg:grid-cols-[300px_1fr]">
        {/* property picker */}
        <div className="flex flex-col gap-2">
          {Object.entries(PROPS).map(([key, p]) => (
            <button key={key} onClick={() => { setProp(key); setRes(null) }}
              className={cn(
                "rounded-xl border bg-card p-3.5 text-left transition-colors hover:border-primary/40",
                prop === key && "border-primary/60 ring-1 ring-primary/30",
              )}>
              <div className="flex items-center gap-2 font-mono text-[13px] font-semibold">
                <FlaskConical className={cn("size-3.5", prop === key ? "text-primary" : "text-muted-foreground")} />
                {p.label}
              </div>
              <p className="mt-1.5 text-xs leading-relaxed text-muted-foreground">{p.blurb}</p>
            </button>
          ))}
        </div>

        {/* harness output */}
        <Card className="flex min-h-[360px] flex-col overflow-hidden p-0">
          <div className="flex items-center gap-2 border-b bg-secondary/60 px-3.5 py-2.5">
            <FileCode2 className="size-3.5 text-muted-foreground" />
            <span className="font-mono text-xs text-muted-foreground">{res?.name ?? `${prop} invariant`}</span>
            <Button size="sm" onClick={run} disabled={busy} className="ml-auto h-7 gap-1.5 text-xs">
              {busy ? <Loader2 className="size-3.5 animate-spin" /> : <Play className="size-3.5" />}
              {busy ? "generating" : "Generate harness"}
            </Button>
          </div>
          <div className="flex-1 overflow-auto p-4 font-mono text-[12.5px] leading-relaxed">
            {!res && !busy && (
              <p className="text-muted-foreground">Press <span className="text-foreground">Generate harness</span> — SCSA lowers the target and scaffolds a real Foundry invariant test.</p>
            )}
            {busy && <p className="text-muted-foreground">lowering target · scaffolding handler…</p>}
            {res && (
              <>
                <pre className="whitespace-pre-wrap text-foreground/90">{res.harness || "// (харнесс не сгенерирован для этой цели/свойства)"}</pre>
                {res.harness && (
                  <div className="mt-3 rounded-md border border-primary/30 bg-primary/5 px-3 py-2 text-[12px] text-muted-foreground">
                    готово → запусти <span className="font-mono text-foreground">forge test --match-contract {res.name?.replace(/\.t\.sol$/, "")}</span> — падение инварианта = подтверждённый эксплойт
                  </div>
                )}
              </>
            )}
          </div>
        </Card>
      </motion.div>
    </motion.div>
  )
}
