import { useState } from "react"
import { motion } from "motion/react"
import { Star, ArrowRight, ScanLine, Loader2, TriangleAlert } from "lucide-react"
import {
  Table, TableBody, TableCell, TableHead, TableHeader, TableRow,
} from "@/components/ui/table"
import { Card } from "@/components/ui/card"
import { Button } from "@/components/ui/button"
import { Input } from "@/components/ui/input"
import { cn } from "@/lib/utils"
import { rise, stagger } from "@/lib/motion"
import { QUEUE, SKIP_CODES, type QState, type QRow } from "@/queue-data"
import { batchScan } from "@/lib/api"

const STATE_PILL: Record<QState, string> = {
  survivor: "border-primary/50 bg-primary/10 text-primary",
  minor: "border-warning/40 bg-warning/10 text-warning",
  clean: "border-success/40 bg-success/10 text-success",
  skip: "border-border text-muted-foreground",
}
const STATE_LABEL: Record<QState, string> = {
  survivor: "survivor", minor: "minor", clean: "clean", skip: "skipped",
}

const STATS: { s: QState; label: string; tone: string }[] = [
  { s: "survivor", label: "Survivors", tone: "text-primary" },
  { s: "minor", label: "Minor", tone: "text-warning" },
  { s: "clean", label: "Clean", tone: "text-success" },
  { s: "skip", label: "Skipped", tone: "text-muted-foreground" },
]

export default function HuntQueue() {
  const [addrs, setAddrs] = useState("")
  const [chain, setChain] = useState("1")
  const [rows, setRows] = useState<QRow[] | null>(null)
  const [busy, setBusy] = useState(false)
  const [err, setErr] = useState("")

  const data = rows ?? QUEUE
  const live = rows !== null
  const n = (s: QState) => data.filter((r) => r.state === s).length
  const skipByCode = data.filter((r) => r.state === "skip").reduce<Record<string, number>>((m, r) => {
    if (r.code) m[r.code] = (m[r.code] ?? 0) + 1
    return m
  }, {})

  async function run() {
    setErr("")
    const targets = addrs.split(/[\s,]+/).map((s) => s.trim()).filter(Boolean)
    if (targets.length === 0) { setErr("Вставь адреса 0x… (по одному в строке)."); return }
    setBusy(true)
    try {
      setRows(await batchScan(targets, Number(chain) || 1))
    } catch (e) {
      setErr(e instanceof Error ? e.message : "batch failed")
    } finally {
      setBusy(false)
    }
  }

  return (
    <motion.div variants={stagger} initial="hidden" animate="show" className="mx-auto max-w-5xl">
      <motion.div variants={rise}>
        <p className="font-mono text-xs font-semibold uppercase tracking-[0.14em] text-primary">Batch funnel</p>
        <h1 className="mt-1 text-2xl font-bold tracking-tight">Hunt queue · {data.length} targets</h1>
        <p className="mt-1.5 max-w-[64ch] text-sm text-muted-foreground">
          A cheap deterministic <span className="font-mono text-foreground">sca --audit</span> pass over the batch.
          Survivors carry a HIGH finding and go straight to a deep hunt; the rest are triaged and reason-coded.
        </p>
      </motion.div>

      {/* batch runner */}
      <motion.div variants={rise}>
        <Card className="mt-5 gap-3 p-4">
          <div className="grid gap-3 sm:grid-cols-[1fr_110px]">
            <textarea
              value={addrs} onChange={(e) => setAddrs(e.target.value)}
              placeholder={"0x… адреса, по одному в строке\n(до 15 за прогон)"}
              rows={3}
              className="w-full resize-y rounded-lg border bg-background px-3 py-2 font-mono text-[13px] outline-none focus:border-primary/50" />
            <div className="flex flex-col gap-2">
              <Input value={chain} onChange={(e) => setChain(e.target.value)} placeholder="chain" className="font-mono" />
              <Button onClick={run} disabled={busy} className="gap-2">
                {busy ? <><Loader2 className="size-4 animate-spin" /> …</> : <><ScanLine className="size-4" /> Run funnel</>}
              </Button>
            </div>
          </div>
          {err && (
            <div className="flex items-start gap-2 rounded-lg border border-destructive/40 bg-destructive/10 px-3 py-2 text-sm text-destructive">
              <TriangleAlert className="mt-0.5 size-4 shrink-0" /> <span>{err}</span>
            </div>
          )}
          <p className="font-mono text-[11px] text-muted-foreground">
            {live ? "живой прогон · sca --audit по каждому адресу" : "демо-данные ниже · вставь адреса и запусти реальную воронку"} · нужен ETHERSCAN_API_KEY на сервере
          </p>
        </Card>
      </motion.div>

      {/* bucket summary */}
      <motion.div variants={rise} className="mt-4 grid grid-cols-2 divide-x divide-y overflow-hidden rounded-xl border bg-card sm:grid-cols-4 sm:divide-y-0">
        {STATS.map((x) => (
          <div key={x.s} className="px-5 py-4">
            <div className={cn("text-[26px] font-extrabold tabular-nums leading-none tracking-tight", x.tone)}>{n(x.s)}</div>
            <div className="mt-1.5 font-mono text-[10px] uppercase tracking-[0.08em] text-muted-foreground">{x.label}</div>
          </div>
        ))}
      </motion.div>

      {/* skip breakdown */}
      {Object.keys(skipByCode).length > 0 && (
        <motion.div variants={rise} className="mt-3 flex flex-wrap items-center gap-2 font-mono text-[11px] text-muted-foreground">
          <span className="uppercase tracking-[0.1em]">Skips:</span>
          {Object.entries(skipByCode).map(([code, count]) => (
            <span key={code} className="rounded-full border px-2.5 py-1">
              <span className="text-foreground">{code}</span> ×{count} · {SKIP_CODES[code] ?? "skip"}
            </span>
          ))}
        </motion.div>
      )}

      <motion.div variants={rise}>
        <Card className="mt-4 overflow-hidden p-0">
          <Table>
            <TableHeader>
              <TableRow className="hover:bg-transparent">
                <TableHead>Target</TableHead>
                <TableHead className="w-[130px]">State</TableHead>
                <TableHead>Detail</TableHead>
                <TableHead className="w-[120px]" />
              </TableRow>
            </TableHeader>
            <TableBody>
              {data.length === 0 && (
                <TableRow className="hover:bg-transparent">
                  <TableCell colSpan={4} className="py-8 text-center text-sm text-muted-foreground">Очередь пуста.</TableCell>
                </TableRow>
              )}
              {data.map((r) => (
                <TableRow key={r.target} className={cn(r.state === "survivor" && "bg-primary/[0.04]")}>
                  <TableCell className="font-mono text-[13px] text-foreground">{r.target}</TableCell>
                  <TableCell>
                    <span className={cn("inline-flex items-center gap-1.5 rounded-full border px-2.5 py-1 font-mono text-[11px]", STATE_PILL[r.state])}>
                      {r.state === "survivor" && <Star className="size-3 fill-current" />}
                      {r.state === "skip" && r.code ? `${STATE_LABEL[r.state]} [${r.code}]` : STATE_LABEL[r.state]}
                    </span>
                  </TableCell>
                  <TableCell className="text-[13px] text-muted-foreground">{r.detail}</TableCell>
                  <TableCell>
                    {r.state === "survivor" ? (
                      <span className="inline-flex items-center gap-1.5 font-mono text-xs text-primary">/hunt <ArrowRight className="size-3.5" /></span>
                    ) : (
                      <span className="font-mono text-[11px] text-muted-foreground/60">—</span>
                    )}
                  </TableCell>
                </TableRow>
              ))}
            </TableBody>
          </Table>
        </Card>
      </motion.div>

      <motion.p variants={rise} className="mt-4 font-mono text-xs text-muted-foreground">
        Deterministic layer runs without the AI analyst — it only engages on survivors, keeping the deep pass cheap.
      </motion.p>
    </motion.div>
  )
}
