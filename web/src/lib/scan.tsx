// Стор результата скана: держит последний ответ бэкенда и умеет его запросить.
// Бэкенд (Go) — тонкий: дёргает бинарь `sca`, отдаёт находки в модели фронта (data.ts).
// Результат кладём в sessionStorage, чтобы переживал навигацию/перезагрузку страницы.
import { createContext, useContext, useState, type ReactNode } from "react"
import type { Finding } from "@/data"

export interface ScanTarget {
  name: string
  source: string
  chain: number
}
export interface ScanResult {
  target: ScanTarget
  findings: Finding[]
}
export interface ScanRequest {
  mode: "address" | "source"
  address?: string
  chain?: number
  source?: string
  name?: string
}

interface ScanCtx {
  result: ScanResult | null
  runScan: (req: ScanRequest) => Promise<ScanResult>
  clear: () => void
}

const Ctx = createContext<ScanCtx | null>(null)
const KEY = "scsa.scan"

function load(): ScanResult | null {
  try {
    const s = sessionStorage.getItem(KEY)
    return s ? (JSON.parse(s) as ScanResult) : null
  } catch {
    return null
  }
}

export function ScanProvider({ children }: { children: ReactNode }) {
  const [result, setResult] = useState<ScanResult | null>(load)

  async function runScan(req: ScanRequest): Promise<ScanResult> {
    const res = await fetch("/api/scan", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(req),
    })
    if (!res.ok) {
      let msg = `scan failed (${res.status})`
      try {
        const j = await res.json()
        if (j?.error) msg = j.error
      } catch {
        /* тело не JSON — оставляем статус */
      }
      throw new Error(msg)
    }
    const data = (await res.json()) as ScanResult
    setResult(data)
    try {
      sessionStorage.setItem(KEY, JSON.stringify(data))
    } catch {
      /* приватный режим / переполнение — не критично */
    }
    return data
  }

  function clear() {
    setResult(null)
    try {
      sessionStorage.removeItem(KEY)
    } catch {
      /* ignore */
    }
  }

  return <Ctx.Provider value={{ result, runScan, clear }}>{children}</Ctx.Provider>
}

export function useScan(): ScanCtx {
  const c = useContext(Ctx)
  if (!c) throw new Error("useScan must be used within <ScanProvider>")
  return c
}
