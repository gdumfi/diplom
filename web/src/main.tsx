import { StrictMode } from "react"
import { createRoot } from "react-dom/client"
import "@fontsource-variable/geist"
import "@fontsource-variable/geist-mono"
import "./index.css"
import App from "./App.tsx"

// brand is dark-first; ThemeToggle reads this and lets the viewer flip it
document.documentElement.classList.add("dark")

createRoot(document.getElementById("root")!).render(
  <StrictMode>
    <App />
  </StrictMode>,
)
