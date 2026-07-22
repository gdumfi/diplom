import { Link, useParams } from "react-router-dom"
import { motion } from "motion/react"
import { ArrowLeft, Check, Play, Download, ShieldAlert, Wrench, Crosshair } from "lucide-react"
import { Tabs, TabsContent, TabsList, TabsTrigger } from "@/components/ui/tabs"
import { Button } from "@/components/ui/button"
import { Card } from "@/components/ui/card"
import {
  Table, TableBody, TableCell, TableHead, TableHeader, TableRow,
} from "@/components/ui/table"
import { cn } from "@/lib/utils"
import {
  FINDINGS, RULE_LABEL, STATUS_LABEL, RULE_DETAIL, FN_FACTS, STORAGE_MAP, TARGET,
  type Finding, type RuleDetail,
} from "@/data"
import { useScan } from "@/lib/scan"

const SEV_TEXT: Record<string, string> = { HIGH: "text-destructive", MED: "text-warning", LOW: "text-muted-foreground" }

function yn(b: boolean) {
  return b ? <span className="text-success">yes</span> : <span className="text-muted-foreground">no</span>
}

// разбор для находок, чьего правила нет в статическом RULE_DETAIL (missing-slippage,
// selfdestruct, delegatecall и т.п.): берём описание прямо из message бинаря
function genericDetail(f: Finding): RuleDetail {
  const proof: RuleDetail["proof"] = [
    { t: "$ sca --address … --audit --json", c: "cmd" },
    { t: `  ${f.rule} · ${f.fn} · L${f.line} · score ${f.score}`, c: "muted" },
  ]
  if (f.status === "klee") proof.push({ t: "  [SYM-CONFIRMED] — path reachable", c: "crit" })
  return {
    root: f.message || "The analyzer flagged this location — see the finding message from sca --json.",
    impact: `Severity ${f.sev}. Review ${f.fn} and confirm exploitability against real funds.`,
    fix: "Add the missing guard, follow checks-effects-interactions, or restrict the caller — per the finding class.",
    confirmLabel: f.status === "klee" ? "KLEE — symbolically confirmed" : "Confirm with KLEE or a broken invariant",
    proof,
    poc: "// Generate a Foundry PoC on the server:\n//   sca --address … --poc\n// (PoC templating lives in the analyzer binary, not the web layer.)",
  }
}

export default function FindingDetail() {
  const { id } = useParams()
  const { result } = useScan()
  const findings = result?.findings ?? FINDINGS
  const target = result?.target ?? TARGET
  const live = !!result
  const f = findings.find((x) => x.id === id) ?? findings[0]
  if (!f) return <div className="mx-auto max-w-5xl py-10 text-sm text-muted-foreground">Finding not found.</div>
  const rd = RULE_DETAIL[f.rule] ?? genericDetail(f)

  return (
    <motion.div initial={{ opacity: 0, y: 12 }} animate={{ opacity: 1, y: 0 }}
      transition={{ type: "spring", stiffness: 130, damping: 22 }} className="mx-auto max-w-5xl">
      <Link to="/app" className="inline-flex items-center gap-1.5 font-mono text-xs text-muted-foreground hover:text-foreground">
        <ArrowLeft className="size-3.5" /> back to findings
      </Link>

      <div className="mt-3 flex flex-wrap items-start gap-4">
        <div>
          <div className="flex items-center gap-3">
            <span className={cn("font-mono text-sm font-bold", SEV_TEXT[f.sev])}>{f.sev}</span>
            <h1 className="text-2xl font-bold tracking-tight">{RULE_LABEL[f.rule] ?? f.rule}</h1>
          </div>
          <p className="mt-1 font-mono text-sm text-muted-foreground">
            {target.name} · <span className="text-foreground">{f.fn}</span> · line {f.line} ·{" "}
            <span className="text-muted-foreground">{STATUS_LABEL[f.status]}</span>
          </p>
        </div>
        <div className="ml-auto flex gap-2">
          <Button variant="outline" className="gap-2"><Play className="size-4" /> Confirm with KLEE</Button>
          <Button className="gap-2"><Download className="size-4" /> Generate PoC</Button>
        </div>
      </div>

      <Tabs defaultValue="overview" className="mt-6">
        <TabsList>
          <TabsTrigger value="overview">Overview</TabsTrigger>
          <TabsTrigger value="facts">IR facts</TabsTrigger>
          <TabsTrigger value="confirm">Confirm</TabsTrigger>
          <TabsTrigger value="poc">PoC</TabsTrigger>
        </TabsList>

        {/* overview */}
        <TabsContent value="overview" className="mt-4 grid gap-4 md:grid-cols-2">
          <Card className="gap-2 p-5 md:col-span-2">
            <div className="flex items-center gap-2 text-sm font-semibold"><ShieldAlert className="size-4 text-destructive" /> Root cause</div>
            <p className="text-sm leading-relaxed text-muted-foreground">{rd.root}</p>
          </Card>
          <Card className="gap-2 p-5">
            <div className="flex items-center gap-2 text-sm font-semibold"><Crosshair className="size-4 text-warning" /> Impact</div>
            <p className="text-sm leading-relaxed text-muted-foreground">{rd.impact}</p>
          </Card>
          <Card className="gap-2 p-5">
            <div className="flex items-center gap-2 text-sm font-semibold"><Wrench className="size-4 text-success" /> Fix</div>
            <p className="text-sm leading-relaxed text-muted-foreground">{rd.fix}</p>
          </Card>
        </TabsContent>

        {/* IR facts (real) */}
        <TabsContent value="facts" className="mt-4 space-y-5">
          {live && (
            <div className="rounded-lg border border-warning/40 bg-warning/10 px-3 py-2 font-mono text-[11px] text-warning">
              sample IR facts below — live <span className="font-semibold">--llm-context</span> extraction isn't wired to /api/scan yet
            </div>
          )}
          <Card className="overflow-hidden p-0">
            <div className="border-b px-4 py-2.5 font-mono text-xs text-muted-foreground">function facts — from <span className="text-foreground">sca --llm-context</span></div>
            <div className="overflow-x-auto">
              <Table>
                <TableHeader><TableRow className="hover:bg-transparent">
                  <TableHead>fn</TableHead><TableHead>params</TableHead><TableHead>writes</TableHead>
                  <TableHead>reads</TableHead><TableHead>ext</TableHead><TableHead>$in</TableHead>
                  <TableHead>auth</TableHead><TableHead>notes</TableHead>
                </TableRow></TableHeader>
                <TableBody>
                  {FN_FACTS.map((r) => (
                    <TableRow key={r.fn} className={cn(f.fn.startsWith(r.fn) && "bg-primary/5")}>
                      <TableCell className="font-mono text-[13px] text-foreground">{r.fn}</TableCell>
                      <TableCell className="font-mono text-[13px]">{r.params}</TableCell>
                      <TableCell className="font-mono text-[13px]">{r.writes}</TableCell>
                      <TableCell className="font-mono text-[13px]">{r.reads}</TableCell>
                      <TableCell className="font-mono text-[13px]">{r.ext}</TableCell>
                      <TableCell className="font-mono text-[13px]">{yn(r.payable)}</TableCell>
                      <TableCell className="font-mono text-[13px]">{yn(r.auth)}</TableCell>
                      <TableCell className="font-mono text-[12px] text-muted-foreground">{r.extra}</TableCell>
                    </TableRow>
                  ))}
                </TableBody>
              </Table>
            </div>
          </Card>
          <Card className="overflow-hidden p-0">
            <div className="border-b px-4 py-2.5 font-mono text-xs text-muted-foreground">storage map — slot → access</div>
            <div className="overflow-x-auto">
              <Table>
                <TableHeader><TableRow className="hover:bg-transparent">
                  <TableHead>slot</TableHead><TableHead>name</TableHead><TableHead>kind</TableHead>
                  <TableHead>writes</TableHead><TableHead>reads</TableHead>
                </TableRow></TableHeader>
                <TableBody>
                  {STORAGE_MAP.map((s) => (
                    <TableRow key={s.slot}>
                      <TableCell className="font-mono text-[13px]">{s.slot}</TableCell>
                      <TableCell className="font-mono text-[13px] text-foreground">{s.name}</TableCell>
                      <TableCell className="font-mono text-[12px] text-muted-foreground">{s.kind}</TableCell>
                      <TableCell className="font-mono text-[12px]">{s.writes}</TableCell>
                      <TableCell className="font-mono text-[12px]">{s.reads}</TableCell>
                    </TableRow>
                  ))}
                </TableBody>
              </Table>
            </div>
          </Card>
        </TabsContent>

        {/* confirm */}
        <TabsContent value="confirm" className="mt-4">
          <Card className="overflow-hidden p-0">
            <div className="border-b bg-secondary/40 px-4 py-2.5 font-mono text-xs text-muted-foreground">{rd.confirmLabel}</div>
            <div className="space-y-1 p-4 font-mono text-[12.5px] leading-relaxed">
              {rd.proof.map((ln, i) => (
                <div key={i} className={cn(
                  "whitespace-pre-wrap",
                  ln.c === "cmd" && "text-foreground",
                  ln.c === "muted" && "text-muted-foreground",
                  ln.c === "crit" && "font-semibold text-destructive",
                  ln.c === "ok" && "font-semibold text-success",
                )}>{ln.t}</div>
              ))}
            </div>
            {f.sev !== "LOW" && (
              <div className="flex items-center gap-2 border-t border-success/30 bg-success/10 px-4 py-2.5 text-xs font-bold text-success">
                <Check className="size-3.5" /> once confirmed → status flips to exploit proven
              </div>
            )}
          </Card>
        </TabsContent>

        {/* poc */}
        <TabsContent value="poc" className="mt-4">
          <Card className="overflow-hidden p-0">
            <div className="flex items-center gap-2 border-b bg-secondary/40 px-4 py-2.5">
              <span className="font-mono text-xs text-muted-foreground">{f.rule}.poc.sol</span>
              <Button size="sm" variant="ghost" className="ml-auto h-7 gap-1.5 text-xs"><Download className="size-3.5" /> save</Button>
            </div>
            <pre className="overflow-x-auto p-4 font-mono text-[12.5px] leading-relaxed text-foreground/90">{rd.poc}</pre>
          </Card>
        </TabsContent>
      </Tabs>
    </motion.div>
  )
}
