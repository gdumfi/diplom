// Тонкие вызовы бэкенда для HuntQueue и Invariants (тот же паттерн, что scan).
import type { QRow } from "@/queue-data"

async function post<T>(url: string, body: unknown): Promise<T> {
  const res = await fetch(url, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  })
  if (!res.ok) {
    let msg = `${url} failed (${res.status})`
    try {
      const j = await res.json()
      if (j?.error) msg = j.error
    } catch {
      /* тело не JSON */
    }
    throw new Error(msg)
  }
  return res.json() as Promise<T>
}

/** Батч-воронка: пачка адресов → строки очереди (survivor/minor/clean/skip). */
export function batchScan(targets: string[], chain: number): Promise<QRow[]> {
  return post<QRow[]>("/api/batch", { targets, chain })
}

export interface InvResult {
  harness: string
  name: string
  report: string
}

/** Генерация Foundry-инвариант-харнесса для цели+свойства. */
export function genInvariant(req: {
  mode: "address" | "source"
  address?: string
  chain?: number
  source?: string
  property: string
}): Promise<InvResult> {
  return post<InvResult>("/api/invariant", req)
}
