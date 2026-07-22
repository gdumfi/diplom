import { useEffect, useState } from "react"
import { Sun, Moon } from "lucide-react"
import { Button } from "@/components/ui/button"

/** Self-contained theme toggle — reads/writes the `dark` class on <html>. */
export function ThemeToggle() {
  const [dark, setDark] = useState(() =>
    document.documentElement.classList.contains("dark"),
  )
  useEffect(() => {
    document.documentElement.classList.toggle("dark", dark)
  }, [dark])
  return (
    <Button
      variant="outline"
      size="icon"
      onClick={() => setDark((d) => !d)}
      aria-label="Toggle color theme"
    >
      {dark ? <Sun className="size-4" /> : <Moon className="size-4" />}
    </Button>
  )
}
