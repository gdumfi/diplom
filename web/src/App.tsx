import { BrowserRouter, Routes, Route } from "react-router-dom"
import { TooltipProvider } from "@/components/ui/tooltip"
import { ScanProvider } from "@/lib/scan"
import Landing from "@/pages/Landing"
import Pricing from "@/pages/Pricing"
import AppShell from "@/components/app-shell"
import Findings from "@/pages/Findings"
import FindingDetail from "@/pages/FindingDetail"
import NewScan from "@/pages/NewScan"
import InstallCli from "@/pages/InstallCli"
import Invariants from "@/pages/Invariants"
import HuntQueue from "@/pages/HuntQueue"
import Settings from "@/pages/Settings"

export default function App() {
  return (
    <BrowserRouter>
      <TooltipProvider delayDuration={200}>
       <ScanProvider>
        <Routes>
          <Route path="/" element={<Landing />} />
          <Route path="/pricing" element={<Pricing />} />
          <Route path="/cli" element={<InstallCli />} />
          <Route path="/app" element={<AppShell />}>
            <Route index element={<Findings />} />
            <Route path="scan" element={<NewScan />} />
            <Route path="finding/:id" element={<FindingDetail />} />
            <Route path="invariants" element={<Invariants />} />
            <Route path="queue" element={<HuntQueue />} />
            <Route path="settings" element={<Settings />} />
          </Route>
        </Routes>
       </ScanProvider>
      </TooltipProvider>
    </BrowserRouter>
  )
}
