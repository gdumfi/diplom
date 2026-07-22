import { Link } from "react-router-dom"
import type { ReactNode } from "react"
import { motion, MotionConfig, type Variants } from "motion/react"
import {
  ArrowRight, Check, ShieldCheck, Boxes, Radar, Zap, GitBranch, FlaskConical, Binary,
} from "lucide-react"
import { Button } from "@/components/ui/button"
import { Badge } from "@/components/ui/badge"
import {
  Accordion, AccordionContent, AccordionItem, AccordionTrigger,
} from "@/components/ui/accordion"
import { cn } from "@/lib/utils"
import { ThemeToggle } from "@/lib/theme"

const spring = { type: "spring", stiffness: 120, damping: 20 } as const
const rise: Variants = { hidden: { opacity: 0, y: 22 }, show: { opacity: 1, y: 0, transition: spring } }
const stagger: Variants = { hidden: {}, show: { transition: { staggerChildren: 0.09 } } }

/* asymmetric bento — varied spans, never a 3-equal row (DESIGN.md §5) */
const FEATURES = [
  { icon: GitBranch, span: "lg:col-span-4", title: "Auto-PoC on a fork", kicker: "The moat",
    body: "SCSA emits a Foundry exploit — attacker contract, victim interface, createSelectFork — that drains the deployed contract and asserts the theft. Paste it straight into a bounty report.",
    tag: "forge test → PASS" },
  { icon: ShieldCheck, span: "lg:col-span-2", title: "Symbolic confirmation",
    body: "KLEE finds a concrete witness for flagged paths. Confirmed findings carry a SYM-CONFIRMED badge; the rest get triaged down.",
    tag: "reachable, not plausible" },
  { icon: FlaskConical, span: "lg:col-span-3", title: "Invariant fuzzing",
    body: "Four money-class properties auto-scaffolded as Foundry harnesses and broken by the fuzzer, with a shrunk counterexample.",
    tag: "solvency · inflation · inflation-erc20 · roundtrip" },
  { icon: Boxes, span: "lg:col-span-3", title: "LLVM IR, not bytecode",
    body: "Analysis runs on compiler-grade IR with EVM semantics intact — SSA, dominator trees, call graphs. A stronger substrate than EVM-opcode tools.",
    tag: "addrspace(1) storage · @scsa.external_call" },
  { icon: Radar, span: "lg:col-span-2", title: "Batch funnel",
    body: "Point it at an address list; a cheap deterministic pass triages thousands and surfaces only the HIGH survivors.",
    tag: "reason-coded skips" },
  { icon: Zap, span: "lg:col-span-4", title: "Neuro-symbolic hunt",
    body: "An AI analyst reasons over compiler facts — never raw text — and every hypothesis it raises is confirmed by KLEE or a broken invariant before it ships. No hallucinated bugs.",
    tag: "grounded in IR · verified before it ships" },
]

const PROVES = [
  { cls: "Reentrancy (CEI violation)", how: "KLEE witness + Foundry PoC" },
  { cls: "Access control", how: "KLEE — attacker reaches a privileged write" },
  { cls: "First-depositor / share-inflation", how: "forge invariant counterexample" },
  { cls: "Rounding creates value", how: "forge round-trip invariant" },
  { cls: "Arbitrary external call", how: "data-flow to attacker-controlled target" },
  { cls: "Oracle spot-price", how: "flash-loan PoC on a mainnet fork" },
]

const STATS = [
  { n: "1.00", l: "Precision / recall", s: "curated 0.8 corpus, 38 files" },
  { n: "0", l: "High false-positives", s: "Seaport · Permit2 · 1inch V5" },
  { n: "4", l: "Forge-proven invariants", s: "money-class, both directions" },
  { n: "100%", l: "Findings ship a proof", s: "counterexample or KLEE witness" },
]

const FAQ = [
  { q: "Is this just another scanner?",
    a: "No. Detection is the easy, free part. SCSA's output is a working exploit that proves the theft on a fork — that's what a bounty pays for." },
  { q: "How is it different from Slither?",
    a: "Slither flags patterns statically and for free. SCSA confirms the path symbolically with KLEE and ships a runnable Foundry PoC — fewer false positives, submittable findings. They're complementary." },
  { q: "Does it replace an audit?",
    a: "No — it accelerates hunters and auditors on the most labor-intensive step: turning a suspicion into a proven exploit. Intent-level logic bugs still need a human." },
  { q: "What does it support today?",
    a: "Solidity 0.8.x, fetched by address from Etherscan or scanned locally. Pre-0.8 contracts are skipped with an explicit reason." },
  { q: "Will it invent bugs?",
    a: "No. Every hypothesis is confirmed by a KLEE witness or a broken invariant, or it's discarded. Nothing ships without a proof." },
]

function ProofPanel() {
  const L = ({ children, className }: { children: ReactNode; className?: string }) => (
    <div className={cn("whitespace-pre-wrap", className)}>{children}</div>
  )
  return (
    <div className="overflow-hidden rounded-xl border bg-card shadow-2xl">
      <div className="flex items-center gap-2 border-b bg-secondary/60 px-3.5 py-2.5">
        <span className="size-2.5 rounded-full bg-destructive" />
        <span className="size-2.5 rounded-full bg-warning" />
        <span className="size-2.5 rounded-full bg-success" />
        <span className="ml-2 font-mono text-xs text-muted-foreground">scsa · exploit-proof</span>
      </div>
      <div className="space-y-1 p-4 font-mono text-[13px] leading-relaxed">
        <L><span className="text-muted-foreground">$</span> scsa audit <span className="text-primary">0x9f2…Vault</span> --emit-invariant inflation-erc20</L>
        <L className="text-muted-foreground">  lowering Solidity → LLVM IR … ok  (solc 0.8.19)</L>
        <L className="text-muted-foreground">  CFG detectors · KLEE oracle · shape = ERC4626</L>
        <L><span className="font-semibold text-destructive">[HIGH]</span> first-depositor share-inflation — <span className="text-primary">Vault.deposit</span></L>
        <L className="text-muted-foreground">  ▸ forge test invariant_noInflationProfit …</L>
        <L><span className="font-semibold text-destructive">    FAILED</span> — attacker withdrew <span className="font-semibold text-destructive">206.06</span> &gt; <span className="text-muted-foreground">201.55</span></L>
        <L className="text-muted-foreground">    seq: deposit(1)·donate(100e18)·victimDeposit(5e18)·redeem()</L>
        <div className="mt-3 inline-flex items-center gap-2 rounded-md border border-success/40 bg-success/10 px-3 py-1.5 text-xs font-bold text-success">
          <Check className="size-3.5" /> EXPLOIT PROVEN · PoC → VaultInflation.t.sol
        </div>
        <L className="mt-1.5 text-muted-foreground">
          <span className="text-muted-foreground">$ </span>
          <span className="inline-block h-3.5 w-2 translate-y-0.5 bg-primary align-middle animate-[blink_1.1s_steps(1)_infinite]" />
        </L>
      </div>
    </div>
  )
}

export default function Landing() {
  return (
    <MotionConfig reducedMotion="user">
      <div className="min-h-[100dvh] bg-background text-foreground">
        {/* nav */}
        <header className="sticky top-0 z-50 border-b bg-background/80 backdrop-blur-md">
          <div className="mx-auto flex h-16 max-w-[1200px] items-center gap-6 px-6">
            <Link to="/" className="flex items-center gap-2.5 font-bold tracking-tight">
              <span className="grid size-7 place-items-center rounded-lg bg-primary font-mono text-sm font-extrabold text-primary-foreground">S</span>
              SCSA
            </Link>
            <nav className="ml-3 hidden items-center gap-6 text-sm text-muted-foreground md:flex">
              <a className="transition-colors hover:text-foreground" href="#proves">What it proves</a>
              <a className="transition-colors hover:text-foreground" href="#features">How it works</a>
              <Link className="transition-colors hover:text-foreground" to="/pricing">Pricing</Link>
              <a className="transition-colors hover:text-foreground" href="#faq">FAQ</a>
            </nav>
            <div className="ml-auto flex items-center gap-2">
              <ThemeToggle />
              <Button asChild><Link to="/app">Open console</Link></Button>
            </div>
          </div>
        </header>

        {/* hero — asymmetric split, single primary CTA */}
        <section className="relative overflow-hidden border-b">
          <div className="pointer-events-none absolute inset-x-0 top-0 h-[560px]"
            style={{ background: "radial-gradient(720px 340px at 22% 4%, color-mix(in srgb, var(--primary) 15%, transparent), transparent 72%), radial-gradient(520px 300px at 88% -8%, color-mix(in srgb, var(--destructive) 9%, transparent), transparent 70%)" }} />
          <motion.div
            variants={stagger} initial="hidden" animate="show"
            className="relative mx-auto grid max-w-[1200px] items-center gap-14 px-6 py-20 lg:grid-cols-[1.05fr_.95fr] lg:py-28">
            <div>
              <motion.div variants={rise}>
                <Badge variant="outline" className="gap-2 rounded-full font-mono text-xs font-normal">
                  <span className="size-1.5 rounded-full bg-success" /> compiler-grade exploit generation
                </Badge>
              </motion.div>
              <motion.h1 variants={rise} className="mt-6 text-[clamp(2.6rem,6vw,4.25rem)] font-extrabold leading-[1.03] tracking-tight">
                Find the bug.<br /><span className="text-primary">Prove it.</span>
              </motion.h1>
              <motion.p variants={rise} className="mt-6 max-w-[46ch] text-lg leading-relaxed text-muted-foreground">
                SCSA compiles Solidity to LLVM IR, hunts vulnerabilities with symbolic execution, and
                hands you a <span className="font-medium text-foreground">working Foundry exploit</span> that
                proves the theft. Bounties pay for proof, not warnings.
              </motion.p>
              <motion.div variants={rise} className="mt-8 flex flex-wrap items-center gap-5">
                <Button asChild size="lg" className="h-11 gap-2 px-5">
                  <Link to="/app">Open the console <ArrowRight className="size-4" /></Link>
                </Button>
                <Link to="/cli" className="inline-flex items-center gap-1.5 font-mono text-sm text-muted-foreground transition-colors hover:text-foreground">
                  <Binary className="size-4" /> or run the CLI
                </Link>
              </motion.div>
              <motion.div variants={rise} className="mt-8 flex flex-wrap gap-x-5 gap-y-2 font-mono text-[13px] text-muted-foreground">
                <span className="inline-flex items-center gap-1.5"><Check className="size-3.5 text-success" /> every finding carries a proof</span>
                <span className="inline-flex items-center gap-1.5"><Check className="size-3.5 text-success" /> runs on a mainnet fork</span>
              </motion.div>
            </div>
            <motion.div variants={rise}><ProofPanel /></motion.div>
          </motion.div>
        </section>

        {/* insight */}
        <div className="border-b bg-secondary/40">
          <div className="mx-auto max-w-[1200px] px-6 py-7">
            <p className="max-w-4xl font-mono text-sm text-muted-foreground">
              <span className="font-semibold text-foreground">Detection is a commodity. Proof is the product.</span>{" "}
              A dozen scanners flag the same suspicious pattern for free — SCSA keeps the compiler's
              precision all the way down to a Foundry test that goes green when the money moves.
            </p>
          </div>
        </div>

        {/* stats — real numbers only (DESIGN.md anti-fabrication) */}
        <section className="border-b">
          <motion.div variants={stagger} initial="hidden" whileInView="show" viewport={{ once: true, margin: "-12%" }}
            className="mx-auto grid max-w-[1200px] gap-px overflow-hidden px-6 py-14 sm:grid-cols-2 lg:grid-cols-4">
            {STATS.map((s) => (
              <motion.div variants={rise} key={s.l} className="px-2 sm:px-6 first:pl-0">
                <div className="text-[clamp(2rem,4vw,2.75rem)] font-extrabold tracking-tight tabular-nums">{s.n}</div>
                <div className="mt-1 text-sm text-foreground">{s.l}</div>
                <div className="mt-0.5 font-mono text-xs text-muted-foreground">{s.s}</div>
              </motion.div>
            ))}
          </motion.div>
        </section>

        {/* features — bento */}
        <section id="features" className="mx-auto max-w-[1200px] px-6 py-24">
          <div className="max-w-2xl">
            <p className="font-mono text-xs font-semibold uppercase tracking-[0.16em] text-primary">The pipeline</p>
            <h2 className="mt-3 text-[clamp(1.9rem,3.6vw,2.6rem)] font-extrabold tracking-tight">
              Every step ends in a proof — or a discarded hypothesis
            </h2>
          </div>
          <motion.div variants={stagger} initial="hidden" whileInView="show" viewport={{ once: true, margin: "-10%" }}
            className="mt-10 grid grid-cols-1 gap-4 lg:grid-cols-6">
            {FEATURES.map((f) => (
              <motion.article variants={rise} key={f.title}
                className={cn("group rounded-xl border bg-card p-6 transition-colors hover:border-primary/40", f.span)}>
                <div className="flex items-center gap-3">
                  <span className="grid size-9 place-items-center rounded-lg border bg-secondary text-primary">
                    <f.icon className="size-4.5" />
                  </span>
                  {f.kicker && <span className="font-mono text-[11px] uppercase tracking-[0.12em] text-primary">{f.kicker}</span>}
                </div>
                <h3 className="mt-4 text-lg font-bold">{f.title}</h3>
                <p className="mt-2 max-w-[52ch] text-sm leading-relaxed text-muted-foreground">{f.body}</p>
                <p className="mt-4 font-mono text-[11px] text-muted-foreground/70">{f.tag}</p>
              </motion.article>
            ))}
          </motion.div>
        </section>

        {/* what it proves */}
        <section id="proves" className="border-y bg-secondary/30">
          <div className="mx-auto max-w-[1200px] px-6 py-24">
            <div className="grid gap-12 lg:grid-cols-[.8fr_1.2fr] lg:items-start">
              <div>
                <p className="font-mono text-xs font-semibold uppercase tracking-[0.16em] text-primary">Coverage</p>
                <h2 className="mt-3 text-[clamp(1.9rem,3.6vw,2.6rem)] font-extrabold tracking-tight">
                  Classes SCSA doesn't just flag — it proves
                </h2>
                <p className="mt-4 max-w-[42ch] text-muted-foreground">
                  Each class ships with the exact mechanism that confirms it. Where a bug needs
                  protocol intent to understand, we say so instead of guessing.
                </p>
              </div>
              <motion.ul variants={stagger} initial="hidden" whileInView="show" viewport={{ once: true, margin: "-10%" }}
                className="divide-y rounded-xl border bg-card">
                {PROVES.map((p) => (
                  <motion.li variants={rise} key={p.cls} className="flex flex-wrap items-center gap-x-4 gap-y-1 px-5 py-4">
                    <span className="font-medium">{p.cls}</span>
                    <span className="ml-auto font-mono text-[12px] text-muted-foreground">{p.how}</span>
                  </motion.li>
                ))}
              </motion.ul>
            </div>
          </div>
        </section>

        {/* faq */}
        <section id="faq" className="mx-auto max-w-[1200px] px-6 py-24">
          <div className="grid gap-12 lg:grid-cols-[.7fr_1.3fr]">
            <div>
              <p className="font-mono text-xs font-semibold uppercase tracking-[0.16em] text-primary">FAQ</p>
              <h2 className="mt-3 text-[clamp(1.9rem,3.6vw,2.6rem)] font-extrabold tracking-tight">
                Straight answers
              </h2>
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
        </section>

        {/* cta */}
        <section className="relative overflow-hidden border-t">
          <div className="pointer-events-none absolute inset-0"
            style={{ background: "radial-gradient(640px 280px at 50% 118%, color-mix(in srgb, var(--primary) 16%, transparent), transparent 70%)" }} />
          <div className="relative mx-auto max-w-[820px] px-6 py-24 text-left sm:text-center">
            <h2 className="text-[clamp(1.9rem,4vw,2.75rem)] font-extrabold tracking-tight">
              Your next finding could come with a receipt.
            </h2>
            <p className="mt-4 max-w-[46ch] text-muted-foreground sm:mx-auto">
              Point SCSA at an address and let it hand you a Foundry test that proves the drain.
            </p>
            <div className="mt-8 sm:flex sm:justify-center">
              <Button asChild size="lg" className="h-11 gap-2 px-5">
                <Link to="/app">Open the console <ArrowRight className="size-4" /></Link>
              </Button>
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
    </MotionConfig>
  )
}
