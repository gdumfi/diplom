import type { ReactNode } from "react"
import { motion } from "motion/react"
import { KeyRound, Network, Boxes, Check } from "lucide-react"
import { Input } from "@/components/ui/input"
import { Button } from "@/components/ui/button"
import { Card } from "@/components/ui/card"
import { rise, stagger } from "@/lib/motion"

function Field({ label, help, children }: { label: string; help?: string; children: ReactNode }) {
  return (
    <div className="grid gap-1.5">
      <label className="text-sm font-medium">{label}</label>
      {children}
      {help && <p className="font-mono text-xs text-muted-foreground">{help}</p>}
    </div>
  )
}

function Connected() {
  return (
    <span className="inline-flex items-center gap-1.5 rounded-full border border-success/40 bg-success/10 px-2.5 py-1 font-mono text-[11px] text-success">
      <Check className="size-3" /> connected
    </span>
  )
}

const REGISTRIES = [
  { ns: "@scsa", url: "registry.scsa.dev", note: "core detectors & invariant properties" },
  { ns: "@shadcn", url: "ui.shadcn.com", note: "default UI registry" },
]

export default function Settings() {
  return (
    <motion.div variants={stagger} initial="hidden" animate="show" className="mx-auto max-w-3xl">
      <motion.div variants={rise}>
        <p className="font-mono text-xs font-semibold uppercase tracking-[0.14em] text-primary">Settings</p>
        <h1 className="mt-1 text-2xl font-bold tracking-tight">Keys &amp; RPC</h1>
        <p className="mt-1.5 text-sm text-muted-foreground">Credentials stay on your machine. SCSA reads them from the environment; the console never uploads them.</p>
      </motion.div>

      {/* API access */}
      <motion.div variants={rise}>
        <Card className="mt-6 gap-5 p-6">
          <div className="flex items-center gap-2.5">
            <span className="grid size-8 place-items-center rounded-lg border bg-secondary text-primary"><KeyRound className="size-4" /></span>
            <h2 className="text-base font-semibold">Source fetching</h2>
            <div className="ml-auto"><Connected /></div>
          </div>
          <Field label="Etherscan API key" help="ETHERSCAN_API_KEY — used to fetch verified sources by address.">
            <Input type="password" defaultValue="••••••••••••••••••••••••" className="font-mono" />
          </Field>
          <Field label="Default chain" help="chainId for --address scans (1 = Ethereum mainnet, 56 = BSC).">
            <Input defaultValue="1" className="max-w-32 font-mono" />
          </Field>
        </Card>
      </motion.div>

      {/* RPC */}
      <motion.div variants={rise}>
        <Card className="mt-4 gap-5 p-6">
          <div className="flex items-center gap-2.5">
            <span className="grid size-8 place-items-center rounded-lg border bg-secondary text-primary"><Network className="size-4" /></span>
            <h2 className="text-base font-semibold">Fork RPC</h2>
            <div className="ml-auto"><Connected /></div>
          </div>
          <Field label="RPC URL" help="RPC_URL — the fork endpoint PoCs run against (createSelectFork). Archive node recommended.">
            <Input type="password" defaultValue="https://eth-mainnet.g.alchemy.com/v2/••••••••••" className="font-mono" />
          </Field>
          <Field label="Fork block" help="0 = latest. For a bounty, pin the block just before the patch.">
            <Input defaultValue="0" className="max-w-40 font-mono" />
          </Field>
        </Card>
      </motion.div>

      {/* registries */}
      <motion.div variants={rise}>
        <Card className="mt-4 gap-4 p-6">
          <div className="flex items-center gap-2.5">
            <span className="grid size-8 place-items-center rounded-lg border bg-secondary text-primary"><Boxes className="size-4" /></span>
            <h2 className="text-base font-semibold">Registries</h2>
          </div>
          <div className="divide-y rounded-lg border">
            {REGISTRIES.map((r) => (
              <div key={r.ns} className="flex flex-wrap items-center gap-x-3 gap-y-1 px-4 py-3">
                <span className="font-mono text-sm font-semibold text-primary">{r.ns}</span>
                <span className="font-mono text-xs text-muted-foreground">{r.url}</span>
                <span className="ml-auto text-xs text-muted-foreground">{r.note}</span>
              </div>
            ))}
          </div>
        </Card>
      </motion.div>

      <motion.div variants={rise} className="mt-6 flex justify-end gap-2">
        <Button variant="ghost">Reset</Button>
        <Button className="gap-2"><Check className="size-4" /> Save changes</Button>
      </motion.div>
    </motion.div>
  )
}
