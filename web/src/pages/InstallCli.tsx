import { useState, type ReactNode } from "react"
import { Link } from "react-router-dom"
import { motion } from "motion/react"
import { ArrowLeft, Terminal, Copy, Check, Container, Hammer, KeyRound, MonitorPlay } from "lucide-react"
import { Card } from "@/components/ui/card"
import { cn } from "@/lib/utils"
import { rise, stagger } from "@/lib/motion"

function Cmd({ children }: { children: string }) {
  const [copied, setCopied] = useState(false)
  return (
    <div className="group relative">
      <pre className="overflow-x-auto rounded-lg border bg-secondary/40 p-3.5 pr-10 font-mono text-[12.5px] leading-relaxed text-foreground/90">{children}</pre>
      <button
        onClick={() => { navigator.clipboard?.writeText(children); setCopied(true); setTimeout(() => setCopied(false), 1200) }}
        className="absolute right-2 top-2 rounded-md border bg-background/70 p-1.5 text-muted-foreground opacity-0 transition-opacity hover:text-foreground group-hover:opacity-100"
        aria-label="copy">
        {copied ? <Check className="size-3.5 text-success" /> : <Copy className="size-3.5" />}
      </button>
    </div>
  )
}

function Section({ icon, title, sub, children }: { icon: ReactNode; title: string; sub?: string; children: ReactNode }) {
  return (
    <Card className="gap-3 p-5">
      <div className="flex items-center gap-2">
        <span className="text-primary">{icon}</span>
        <h2 className="text-base font-semibold">{title}</h2>
      </div>
      {sub && <p className="-mt-1 text-sm text-muted-foreground">{sub}</p>}
      {children}
    </Card>
  )
}

export default function InstallCli() {
  return (
    <motion.div variants={stagger} initial="hidden" animate="show" className="mx-auto max-w-3xl">
      <motion.div variants={rise}>
        <Link to="/" className="inline-flex items-center gap-1.5 font-mono text-xs text-muted-foreground hover:text-foreground">
          <ArrowLeft className="size-3.5" /> back
        </Link>
        <p className="mt-3 font-mono text-xs font-semibold uppercase tracking-[0.14em] text-primary">CLI</p>
        <h1 className="mt-1 flex items-center gap-2 text-2xl font-bold tracking-tight">
          <Terminal className="size-6 text-primary" /> Run SCSA in your terminal
        </h1>
        <p className="mt-1.5 text-sm text-muted-foreground">
          The core is a single binary — audit, KLEE confirmation, PoC generation and the TUI. Free forever.
        </p>
      </motion.div>

      <div className="mt-6 space-y-4">
        <motion.div variants={rise}>
          <Section icon={<Container className="size-4" />} title="Install — Docker (recommended)"
            sub="The binary compiles Solidity via a solc container, so it ships with Docker. (Public image coming; for now build it once.)">
            <Cmd>{`git clone <repo> && cd solidity-llvm
docker build -t scsa .            # builds the analyzer image

# a tiny 'sca' wrapper on your PATH:
cat > ~/.local/bin/sca <<'EOF'
#!/usr/bin/env bash
exec docker run --rm -i \\
  -v "$PWD":/work -w /work \\
  -v /var/run/docker.sock:/var/run/docker.sock \\
  -e ETHERSCAN_API_KEY scsa "$@"
EOF
chmod +x ~/.local/bin/sca`}</Cmd>
          </Section>
        </motion.div>

        <motion.div variants={rise}>
          <Section icon={<Hammer className="size-4" />} title="Install — from source"
            sub="Needs LLVM 18 + CMake + Ninja. Produces build/src/solidity-llvm.">
            <Cmd>{`cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target solidity-llvm
# optional: symlink 'sca' → the binary
ln -s "$PWD/build/src/solidity-llvm" ~/.local/bin/sca`}</Cmd>
          </Section>
        </motion.div>

        <motion.div variants={rise}>
          <Section icon={<Terminal className="size-4" />} title="Quickstart">
            <Cmd>{`sca -i Vault.sol --audit               # audit a local file
sca --address 0x… --chain 1 --audit   # audit a verified contract
sca -i Vault.sol --audit --json       # machine-readable output
sca -i Vault.sol --poc                # generate a Foundry PoC
sca -i Vault.sol --llm-context        # IR facts pack for an LLM`}</Cmd>
          </Section>
        </motion.div>

        <motion.div variants={rise}>
          <Section icon={<MonitorPlay className="size-4" />} title="Launch the TUI"
            sub="A terminal UI over the same engine — enter a target, browse findings, pull PoC & IR facts.">
            <Cmd>{`sca tui`}</Cmd>
            <p className="text-xs text-muted-foreground">vim keys · <span className="font-mono">j/k</span> navigate · <span className="font-mono">enter</span> open · <span className="font-mono">q</span> quit</p>
          </Section>
        </motion.div>

        <motion.div variants={rise}>
          <Section icon={<KeyRound className="size-4" />} title="Etherscan key (for --address)"
            sub="Scanning by address fetches verified source. A free v2 key covers ETH / Base / Arbitrum / OP / BSC / Polygon.">
            <Cmd>{`export ETHERSCAN_API_KEY=…   # https://etherscan.io/apis`}</Cmd>
          </Section>
        </motion.div>
      </div>

      <motion.p variants={rise} className={cn("mt-6 text-center font-mono text-xs text-muted-foreground")}>
        prefer a UI? <Link to="/app" className="text-primary hover:underline">open the web console →</Link>
      </motion.p>
    </motion.div>
  )
}
