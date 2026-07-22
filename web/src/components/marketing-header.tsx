import { Link } from "react-router-dom"
import { Button } from "@/components/ui/button"
import { ThemeToggle } from "@/lib/theme"

export function MarketingHeader() {
  return (
    <header className="sticky top-0 z-50 border-b bg-background/80 backdrop-blur-md">
      <div className="mx-auto flex h-16 max-w-[1200px] items-center gap-6 px-6">
        <Link to="/" className="flex items-center gap-2.5 font-bold tracking-tight">
          <span className="grid size-7 place-items-center rounded-lg bg-primary font-mono text-sm font-extrabold text-primary-foreground">S</span>
          SCSA
        </Link>
        <nav className="ml-3 hidden items-center gap-6 text-sm text-muted-foreground md:flex">
          <Link className="transition-colors hover:text-foreground" to="/#features">How it works</Link>
          <Link className="transition-colors hover:text-foreground" to="/pricing">Pricing</Link>
          <Link className="transition-colors hover:text-foreground" to="/#faq">FAQ</Link>
        </nav>
        <div className="ml-auto flex items-center gap-2">
          <ThemeToggle />
          <Button asChild><Link to="/app">Open console</Link></Button>
        </div>
      </div>
    </header>
  )
}
