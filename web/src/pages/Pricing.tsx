import { Link } from "react-router-dom"
import { motion } from "motion/react"
import { Check, ArrowRight } from "lucide-react"
import { Button } from "@/components/ui/button"
import {
  Accordion, AccordionContent, AccordionItem, AccordionTrigger,
} from "@/components/ui/accordion"
import { cn } from "@/lib/utils"
import { rise, stagger } from "@/lib/motion"
import { MarketingHeader } from "@/components/marketing-header"

const TIERS = [
  {
    tier: "Hunter", price: "$0", unit: "forever", featured: false,
    desc: "Solo researchers and the open-source core.",
    cta: "Install the CLI", to: "/cli",
    feats: ["Full CLI: audit · KLEE · PoC", "All four invariant harnesses", "Etherscan fetch by address", "JSON & SARIF output"],
  },
  {
    tier: "Team", price: "$79", unit: "seat · month", featured: true,
    desc: "The console, batch scale, fork PoCs.",
    cta: "Start 14-day trial", to: "/app",
    feats: ["Everything in Hunter", "Web console & triage queue", "Batch funnel over address lists", "One-click fork PoC + managed RPC", "CI integration (GitHub / SARIF)"],
  },
  {
    tier: "Enterprise", price: "Custom", unit: "", featured: false,
    desc: "Audit shops & protocol security teams.",
    cta: "Talk to us", to: "/app",
    feats: ["Self-hosted / air-gapped", "Private detector & invariant registries", "Audit workflow & sign-off", "SLA & priority support"],
  },
]

const FAQ = [
  { q: "Is the CLI really free?", a: "Yes — the core (audit, KLEE lowering, PoC and all four invariant harnesses) is open and free forever. You pay for the console, batch scale, and managed fork automation." },
  { q: "What counts as a seat?", a: "One researcher. Seats share a workspace, scan history, and the triage queue. Add or remove them any time; billing is prorated." },
  { q: "Do you take a cut of bounties?", a: "No. What you find and submit is yours. We charge for the tooling, never a percentage of your payouts." },
  { q: "Can I self-host?", a: "Enterprise runs fully on-prem or air-gapped, with private registries for your own detectors and invariant properties." },
]

export default function Pricing() {
  return (
    <div className="min-h-[100dvh] bg-background text-foreground">
      <MarketingHeader />

      <section className="relative overflow-hidden border-b">
        <div className="pointer-events-none absolute inset-x-0 top-0 h-72"
          style={{ background: "radial-gradient(640px 260px at 50% -40%, color-mix(in srgb, var(--primary) 13%, transparent), transparent 72%)" }} />
        <div className="relative mx-auto max-w-[1200px] px-6 py-20 text-center">
          <p className="font-mono text-xs font-semibold uppercase tracking-[0.16em] text-primary">Pricing</p>
          <h1 className="mt-3 text-[clamp(2.2rem,5vw,3.4rem)] font-extrabold tracking-tight">Start free. Pay when it earns.</h1>
          <p className="mx-auto mt-4 max-w-[52ch] text-lg text-muted-foreground">
            The core CLI is open and free forever. Pay for the console, batch scale, and fork automation
            when you're hunting for real.
          </p>
        </div>
      </section>

      <section className="mx-auto max-w-[1200px] px-6 py-16">
        <motion.div variants={stagger} initial="hidden" animate="show"
          className="grid items-start gap-5 lg:grid-cols-3">
          {TIERS.map((t) => (
            <motion.div variants={rise} key={t.tier}
              className={cn(
                "relative flex flex-col rounded-2xl border bg-card p-7",
                t.featured && "border-primary/55 shadow-[0_28px_70px_-34px_color-mix(in_srgb,var(--primary)_60%,transparent)]",
              )}>
              {t.featured && (
                <span className="absolute -top-2.5 right-6 rounded-full bg-primary px-2.5 py-1 font-mono text-[10px] font-bold tracking-wide text-primary-foreground">
                  MOST HUNTERS
                </span>
              )}
              <p className="font-mono text-xs uppercase tracking-[0.14em] text-primary">{t.tier}</p>
              <div className="mt-3 flex items-baseline gap-1.5">
                <span className="text-4xl font-extrabold tracking-tight">{t.price}</span>
                {t.unit && <span className="text-sm text-muted-foreground">/ {t.unit}</span>}
              </div>
              <p className="mt-2 text-sm text-muted-foreground">{t.desc}</p>
              <ul className="mt-6 flex flex-1 flex-col gap-3">
                {t.feats.map((x) => (
                  <li key={x} className="flex gap-2.5 text-sm">
                    <Check className="mt-0.5 size-4 shrink-0 text-success" /> {x}
                  </li>
                ))}
              </ul>
              <Button asChild variant={t.featured ? "default" : "outline"} className="mt-7 w-full gap-2">
                <Link to={t.to}>{t.cta} {t.featured && <ArrowRight className="size-4" />}</Link>
              </Button>
            </motion.div>
          ))}
        </motion.div>

        <p className="mt-8 text-center font-mono text-xs text-muted-foreground">
          No bounty cut · no per-finding fees · cancel any time.
        </p>
      </section>

      <section className="border-t bg-secondary/30">
        <div className="mx-auto max-w-[1200px] px-6 py-20">
          <div className="grid gap-12 lg:grid-cols-[.7fr_1.3fr]">
            <div>
              <p className="font-mono text-xs font-semibold uppercase tracking-[0.16em] text-primary">Billing FAQ</p>
              <h2 className="mt-3 text-[clamp(1.7rem,3.2vw,2.3rem)] font-extrabold tracking-tight">Before you ask</h2>
            </div>
            <Accordion type="single" collapsible className="w-full">
              {FAQ.map((f, i) => (
                <AccordionItem key={f.q} value={`q${i}`}>
                  <AccordionTrigger className="text-left text-base font-semibold">{f.q}</AccordionTrigger>
                  <AccordionContent className="max-w-[62ch] text-sm leading-relaxed text-muted-foreground">{f.a}</AccordionContent>
                </AccordionItem>
              ))}
            </Accordion>
          </div>
        </div>
      </section>

      <footer className="border-t bg-secondary/40">
        <div className="mx-auto flex max-w-[1200px] flex-wrap items-center gap-3 px-6 py-8 font-mono text-xs text-muted-foreground">
          <span className="flex items-center gap-2 font-semibold text-foreground">
            <span className="grid size-5 place-items-center rounded bg-primary text-[10px] font-extrabold text-primary-foreground">S</span> SCSA
          </span>
          <span>© 2026 SCSA</span>
          <span className="ml-auto">Proof over pattern.</span>
        </div>
      </footer>
    </div>
  )
}
