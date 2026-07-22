import { useState } from "react"
import { useNavigate } from "react-router-dom"
import { motion } from "motion/react"
import { ScanLine, ArrowRight, Upload, GitBranch, Wallet, Loader2, TriangleAlert } from "lucide-react"
import { Tabs, TabsContent, TabsList, TabsTrigger } from "@/components/ui/tabs"
import { Input } from "@/components/ui/input"
import { Button } from "@/components/ui/button"
import { Card } from "@/components/ui/card"
import { useScan } from "@/lib/scan"

const STEPS = ["lower → LLVM IR", "CFG detectors", "KLEE confirm", "invariant fuzz", "Foundry PoC"]

export default function NewScan() {
  const nav = useNavigate()
  const { runScan } = useScan()

  const [tab, setTab] = useState("address")
  const [address, setAddress] = useState("")
  const [chain, setChain] = useState("1")
  const [source, setSource] = useState("")
  const [fileName, setFileName] = useState("")
  const [busy, setBusy] = useState(false)
  const [err, setErr] = useState("")

  async function onFile(e: React.ChangeEvent<HTMLInputElement>) {
    const f = e.target.files?.[0]
    if (!f) return
    setFileName(f.name)
    setSource(await f.text())
    setErr("")
  }

  async function run() {
    setErr("")
    try {
      if (tab === "repo") {
        setErr("Repo scanning isn't wired to the backend yet — use an address or upload a .sol.")
        return
      }
      const req =
        tab === "address"
          ? { mode: "address" as const, address: address.trim(), chain: Number(chain) || 1 }
          : { mode: "source" as const, source, name: fileName || "contract.sol" }

      if (req.mode === "address" && !/^0x[0-9a-fA-F]{40}$/.test(req.address)) {
        setErr("Enter a valid 0x… address (40 hex chars).")
        return
      }
      if (req.mode === "source" && !source.trim()) {
        setErr("Drop a .sol file first.")
        return
      }

      setBusy(true)
      await runScan(req)
      nav("/app")
    } catch (e) {
      setErr(e instanceof Error ? e.message : "scan failed")
    } finally {
      setBusy(false)
    }
  }

  return (
    <motion.div initial={{ opacity: 0, y: 12 }} animate={{ opacity: 1, y: 0 }}
      transition={{ type: "spring", stiffness: 130, damping: 22 }} className="mx-auto max-w-2xl">
      <p className="font-mono text-xs font-semibold uppercase tracking-[0.14em] text-primary">New scan</p>
      <h1 className="mt-1 text-2xl font-bold tracking-tight">Point SCSA at a target</h1>
      <p className="mt-1 text-sm text-muted-foreground">Paste a verified address, upload a contract, or connect a repo. Pre-0.8 targets are skipped automatically.</p>

      <Card className="mt-6 p-5">
        <Tabs value={tab} onValueChange={setTab}>
          <TabsList>
            <TabsTrigger value="address" className="gap-1.5"><Wallet className="size-3.5" /> By address</TabsTrigger>
            <TabsTrigger value="upload" className="gap-1.5"><Upload className="size-3.5" /> Upload .sol</TabsTrigger>
            <TabsTrigger value="repo" className="gap-1.5"><GitBranch className="size-3.5" /> Connect repo</TabsTrigger>
          </TabsList>

          <TabsContent value="address" className="mt-4 space-y-3">
            <div className="grid gap-3 sm:grid-cols-[1fr_120px]">
              <Input placeholder="0x… verified contract address" value={address}
                onChange={(e) => setAddress(e.target.value)} className="font-mono" />
              <Input placeholder="chain" value={chain}
                onChange={(e) => setChain(e.target.value)} className="font-mono" />
            </div>
            <p className="font-mono text-xs text-muted-foreground">Source is fetched from Etherscan; the exact solc version is used. (1=Ethereum · 8453=Base · 42161=Arbitrum)</p>
          </TabsContent>

          <TabsContent value="upload" className="mt-4">
            <label className="grid cursor-pointer place-items-center gap-2 rounded-lg border border-dashed py-10 text-center transition-colors hover:border-primary/50">
              <input type="file" accept=".sol" className="hidden" onChange={onFile} />
              <Upload className="size-6 text-muted-foreground" />
              {fileName
                ? <p className="text-sm"><span className="font-mono text-foreground">{fileName}</span> · {source.length} bytes loaded</p>
                : <p className="text-sm text-muted-foreground">Drop a <span className="font-mono text-foreground">.sol</span> file here (single self-contained file)</p>}
            </label>
          </TabsContent>

          <TabsContent value="repo" className="mt-4 space-y-3">
            <Input placeholder="github.com/org/protocol" className="font-mono" disabled />
            <p className="font-mono text-xs text-muted-foreground">Repo scanning needs a checked-out project (foundry/hardhat) — not wired to the web backend yet.</p>
          </TabsContent>
        </Tabs>

        {err && (
          <div className="mt-4 flex items-start gap-2 rounded-lg border border-destructive/40 bg-destructive/10 px-3 py-2.5 text-sm text-destructive">
            <TriangleAlert className="mt-0.5 size-4 shrink-0" /> <span>{err}</span>
          </div>
        )}

        <div className="mt-5 flex items-center justify-between border-t pt-4">
          <div className="flex flex-wrap items-center gap-1.5 font-mono text-[11px] text-muted-foreground">
            {STEPS.map((s, i) => (
              <span key={s} className="flex items-center gap-1.5">
                {i > 0 && <span className="text-primary/60">→</span>}{s}
              </span>
            ))}
          </div>
          <Button className="gap-2" onClick={run} disabled={busy}>
            {busy
              ? <><Loader2 className="size-4 animate-spin" /> Scanning…</>
              : <><ScanLine className="size-4" /> Run scan <ArrowRight className="size-4" /></>}
          </Button>
        </div>
      </Card>

      <p className="mt-3 text-center font-mono text-[11px] text-muted-foreground">
        runs the real <span className="text-foreground">sca --audit --json</span> via the Go backend · address mode needs ETHERSCAN_API_KEY on the server
      </p>
    </motion.div>
  )
}
