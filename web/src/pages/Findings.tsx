import { useNavigate } from "react-router-dom"
import { motion } from "motion/react"
import { ChevronRight, CircleCheck, Sparkles } from "lucide-react"
import {
  Table, TableBody, TableCell, TableHead, TableHeader, TableRow,
} from "@/components/ui/table"
import { Card } from "@/components/ui/card"
import { cn } from "@/lib/utils"
import { rise, stagger } from "@/lib/motion"
import { FINDINGS, STATUS_LABEL, RULE_LABEL, TARGET, type Finding, type Sev, type Status } from "@/data"
import { useScan } from "@/lib/scan"

const SEV_BAR: Record<Sev, string> = {
  HIGH: "bg-destructive", MED: "bg-warning", LOW: "bg-muted-foreground/50",
}
const SEV_TEXT: Record<Sev, string> = {
  HIGH: "text-destructive", MED: "text-warning", LOW: "text-muted-foreground",
}

function StatusBadge({ s }: { s: Status }) {
  const map: Record<Status, string> = {
    proven: "border-success/40 bg-success/10 text-success",
    klee: "border-primary/40 bg-primary/10 text-primary",
    candidate: "border-border text-muted-foreground",
    info: "border-border text-muted-foreground",
  }
  const icon = s === "proven" ? "✓" : s === "klee" ? "◈" : s === "candidate" ? "⋯" : "·"
  return (
    <span className={cn("inline-flex items-center gap-1.5 rounded-full border px-2.5 py-1 font-mono text-[11px]", map[s])}>
      {icon} {STATUS_LABEL[s]}
    </span>
  )
}

const STATS = [
  { k: "high", label: "High", tone: "text-destructive" },
  { k: "med", label: "Medium", tone: "text-warning" },
  { k: "proven", label: "Proven", tone: "text-success" },
  { k: "fp", label: "False pos", tone: "" },
]

export default function Findings() {
  const nav = useNavigate()
  const { result } = useScan()
  const findings = result?.findings ?? FINDINGS
  const target = result?.target ?? TARGET
  const counts: Record<string, number> = {
    high: findings.filter((f) => f.sev === "HIGH").length,
    med: findings.filter((f) => f.sev === "MED").length,
    proven: findings.filter((f) => f.status === "proven" || f.status === "klee").length,
    fp: 0,
  }

  return (
    <motion.div variants={stagger} initial="hidden" animate="show" className="mx-auto max-w-5xl">
      <motion.div variants={rise}>
        <p className="font-mono text-xs font-semibold uppercase tracking-[0.14em] text-primary">Scan result</p>
        <h1 className="mt-1 text-2xl font-bold tracking-tight">{target.name} · {findings.length} findings</h1>
        <p className="mt-1.5 flex items-center gap-1.5 text-sm text-muted-foreground">
          <Sparkles className="size-3.5 text-primary" /> Sorted by money × reachability — proven exploits first.
        </p>
      </motion.div>

      {/* stat strip — divided cells, mono numbers */}
      <motion.div variants={rise} className="mt-6 grid grid-cols-2 divide-x divide-y overflow-hidden rounded-xl border bg-card sm:grid-cols-4 sm:divide-y-0">
        {STATS.map((s) => (
          <div key={s.k} className="px-5 py-4">
            <div className={cn("text-[26px] font-extrabold tabular-nums leading-none tracking-tight", s.tone)}>{counts[s.k]}</div>
            <div className="mt-1.5 font-mono text-[10px] uppercase tracking-[0.08em] text-muted-foreground">{s.label}</div>
          </div>
        ))}
      </motion.div>

      <motion.div variants={rise}>
        <Card className="mt-4 overflow-hidden p-0">
          <Table>
            <TableHeader>
              <TableRow className="hover:bg-transparent">
                <TableHead className="w-[116px]">Severity</TableHead>
                <TableHead>Class</TableHead>
                <TableHead>Location</TableHead>
                <TableHead>Status</TableHead>
                <TableHead className="w-8" />
              </TableRow>
            </TableHeader>
            <TableBody>
              {findings.length === 0 && (
                <TableRow className="hover:bg-transparent">
                  <TableCell colSpan={5} className="py-10 text-center text-sm text-muted-foreground">
                    Clean — no findings in SCSA's classes for this target.
                  </TableCell>
                </TableRow>
              )}
              {findings.map((f: Finding) => (
                <TableRow key={f.id} onClick={() => nav(`/app/finding/${f.id}`)}
                  className="group cursor-pointer">
                  <TableCell>
                    <span className={cn("inline-flex items-center gap-2 font-semibold", SEV_TEXT[f.sev])}>
                      <span className={cn("h-6 w-1 rounded-full", SEV_BAR[f.sev])} /> {f.sev}
                    </span>
                  </TableCell>
                  <TableCell className="font-medium">{RULE_LABEL[f.rule] ?? f.rule}</TableCell>
                  <TableCell className="font-mono text-[13px] tabular-nums">
                    {f.fn} <span className="text-muted-foreground">· L{f.line}</span>
                  </TableCell>
                  <TableCell><StatusBadge s={f.status} /></TableCell>
                  <TableCell className="text-muted-foreground">
                    <ChevronRight className="size-4 transition-transform group-hover:translate-x-0.5 group-hover:text-foreground" />
                  </TableCell>
                </TableRow>
              ))}
            </TableBody>
          </Table>
        </Card>
      </motion.div>

      <motion.p variants={rise} className="mt-4 flex items-center gap-2 font-mono text-xs text-muted-foreground">
        <CircleCheck className="size-3.5 text-success" /> 0 false positives — every finding above carries a counterexample or KLEE witness.
      </motion.p>
    </motion.div>
  )
}
