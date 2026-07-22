import { Link, NavLink, Outlet } from "react-router-dom"
import {
  LayoutList, ScanLine, FlaskConical, ListTree, KeyRound, Lock, RotateCw,
} from "lucide-react"
import { ThemeToggle } from "@/lib/theme"
import { Badge } from "@/components/ui/badge"
import { Button } from "@/components/ui/button"
import { cn } from "@/lib/utils"
import { TARGET } from "@/data"

const NAV = [
  { to: "/app", end: true, icon: LayoutList, label: "Findings", kbd: "F" },
  { to: "/app/scan", icon: ScanLine, label: "New scan", kbd: "S" },
  { to: "/app/invariants", icon: FlaskConical, label: "Invariants", kbd: "I" },
  { to: "/app/queue", icon: ListTree, label: "Hunt queue", kbd: "Q" },
]

const linkBase =
  "relative flex items-center gap-2.5 rounded-md px-2.5 py-2 text-sm font-medium text-muted-foreground transition-colors hover:text-foreground"

export default function AppShell() {
  return (
    <div className="grid min-h-[100dvh] grid-cols-1 bg-background text-foreground md:grid-cols-[236px_1fr]">
      {/* sidebar */}
      <aside className="hidden flex-col border-r bg-sidebar md:flex">
        <div className="flex h-16 items-center gap-2.5 border-b px-5 font-bold tracking-tight">
          <span className="grid size-7 place-items-center rounded-lg bg-primary font-mono text-sm font-extrabold text-primary-foreground">S</span>
          <Link to="/">SCSA</Link>
          <Badge variant="outline" className="ml-auto rounded-full font-mono text-[10px]">console</Badge>
        </div>
        <nav className="flex flex-col gap-1 p-3">
          <p className="px-2 pb-1 pt-2 font-mono text-[10px] uppercase tracking-[0.12em] text-muted-foreground">Workspace</p>
          {NAV.map((n) => (
            <NavLink key={n.to} to={n.to} end={n.end}
              className={({ isActive }) => cn(
                linkBase,
                isActive &&
                  "bg-secondary text-foreground before:absolute before:inset-y-1.5 before:left-0 before:w-0.5 before:rounded-full before:bg-primary",
              )}>
              <n.icon className="size-4" /> {n.label}
              <kbd className="ml-auto font-mono text-[10px] text-muted-foreground">{n.kbd}</kbd>
            </NavLink>
          ))}
          <p className="px-2 pb-1 pt-4 font-mono text-[10px] uppercase tracking-[0.12em] text-muted-foreground">Account</p>
          <NavLink to="/app/settings" className={({ isActive }) => cn(linkBase, isActive && "bg-secondary text-foreground before:absolute before:inset-y-1.5 before:left-0 before:w-0.5 before:rounded-full before:bg-primary")}>
            <KeyRound className="size-4" /> Keys &amp; RPC
          </NavLink>
        </nav>
        <div className="mt-auto flex items-center gap-2 border-t p-4 font-mono text-[11px] text-muted-foreground">
          <span className="size-1.5 rounded-full bg-success" /> engine ready · Proof over pattern.
        </div>
      </aside>

      {/* main column */}
      <div className="flex min-w-0 flex-col">
        <header className="flex h-16 items-center gap-3 border-b px-5">
          <Lock className="size-3.5 shrink-0 text-success" />
          <span className="truncate font-mono text-sm text-muted-foreground">
            {TARGET.name} · <span className="text-foreground">{TARGET.source}</span> · chain {TARGET.chain}
          </span>
          <div className="ml-auto flex items-center gap-2">
            <Button variant="outline" size="sm" className="hidden gap-1.5 sm:inline-flex">
              <RotateCw className="size-3.5" /> Re-scan
            </Button>
            <ThemeToggle />
          </div>
        </header>
        <main className="relative min-w-0 flex-1 p-6">
          <div className="pointer-events-none absolute inset-x-0 top-0 h-56"
            style={{ background: "radial-gradient(620px 200px at 28% -30%, color-mix(in srgb, var(--primary) 9%, transparent), transparent 72%)" }} />
          <div className="relative"><Outlet /></div>
        </main>
      </div>
    </div>
  )
}
