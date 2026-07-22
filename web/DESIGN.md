# Design System: SCSA — Proof-Grade Smart-Contract Security

Generated with the `taste-design` skill (anti-slop standards), calibrated to the subject:
a compiler-grade exploit-proving tool for bug-bounty hunters. Immunefi-caliber production
value, but our own identity — gold-on-ink, not purple/neon.

## 1. Visual Theme & Atmosphere
A high-stakes security war-room rendered with a compiler's precision. Density leans dense
(6/10) — this is an instrument, not a brochure — with confident **asymmetric** composition
(variance 7/10) and fluid spring motion (6/10). The mood is clinical and expensive: deep
blue-black ground, a single disciplined gold that reads as *the payout / the proof*, and
mono type doing the load-bearing data work. Nothing screams; hierarchy comes from weight,
color, and negative space.

## 2. Color Palette & Roles
- **Ink Ground** (#0B0F16) — primary background; blue-biased near-black, never pure #000
- **Ink Raise** (#0E141D) — alternating section ground
- **Surface Slate** (#131A24) — card / container fill
- **Surface Slate 2** (#18212E) — nested surfaces, code blocks
- **Hairline** (#222D3B) — 1px structural borders, dividers
- **Ice Text** (#E7ECF3) — primary text
- **Steel Muted** (#93A0B2) — secondary text, metadata, descriptions
- **Assay Gold** (#E6B34A) — THE single accent (≈62% saturation). CTAs, active state,
  focus ring, "proven" emphasis. Reads as bounty/proof, not decoration.
- Semantic (not the accent — state only): **Critical** (#FB5A5A), **Verified** (#45C08A),
  **Watch** (#E4A13C).
Light theme mirrors every token (Canvas #F6F7F9, Ink text #121822, same gold).

## 3. Typography Rules
- **Display + Body:** `Geist` (npm `geist`) — track-tight headlines, weight-driven
  hierarchy, body at ~65ch, relaxed leading. Inter is BANNED.
- **Data / Code / Numbers:** `Geist Mono` — every counterexample, address, slot, metric,
  timestamp. At this density, numbers are mono, always, with `tabular-nums`.
- **No serif anywhere** (this is software UI).
- Headline scale via `clamp()`; body min 1rem.

## 4. Component Stylings
- **Buttons:** flat, no outer glow. Gold fill for the single primary; ghost/outline for the
  rest. Tactile `translateY(-1px)` on hover, press-in on active. 44px min tap target.
- **Cards:** moderate radius (0.75–1rem — precise, not pillowy) with a diffused shadow tinted
  to the ink hue. Used only where elevation encodes hierarchy; in dense lists, prefer
  border-top dividers and negative space over card walls.
- **Inputs:** label above, helper/error below, gold focus ring. No floating labels.
- **Loaders:** skeletal shimmer matching the exact row/panel it replaces — never a spinner.
- **Badges/Chips:** encode state in form as well as text — severity stripe, `SYM-CONFIRMED`,
  `exploit proven` each own a color and a glyph.

## 5. Layout Principles
- CSS Grid first; max-width containment ~1200px, generous internal padding.
- **Hero is asymmetric split** (copy left, live proof panel right) — never centered.
- The generic 3-equal-column feature row is BANNED — use a 2-column zig-zag / asymmetric
  grid / bento with varied cell sizes.
- Every element owns its spatial zone — no overlap, no absolute stacking of content.
- Full-height sections use `min-h-[100dvh]`, never `h-screen`.
- Mobile (<768px): all multi-column collapses to one; zero horizontal scroll.

## 6. Motion & Interaction
- Spring physics (stiffness ~120, damping ~20) — weighty, premium; no linear easing.
- Staggered cascade on section reveal (waterfall, not instant mount).
- One perpetual micro-loop, used sparingly: the terminal cursor blink + a slow shimmer sweep
  across the "EXPLOIT PROVEN" stamp. Not everywhere.
- Animate `transform`/`opacity` only. Respect `prefers-reduced-motion`.

## 7. Anti-Patterns (Banned)
- No emojis anywhere.
- No `Inter`, no generic serif, no pure `#000000`.
- No purple/blue neon, no outer-glow shadows, no oversaturated accent, no gradient-text on
  large headers.
- No 3-equal-column card rows.
- **No fabricated metrics.** Never invent "$X paid", "N programs", uptime %, response times.
  Use only real SCSA numbers (P/R on the curated corpus, 0 HIGH false-positives on
  Seaport/Permit2/1inch, the 206.06 > 201.55 counterexample, 4 forge-proven invariant
  classes) or an explicit `[placeholder]` label. Honesty is a product value here.
- No AI copy clichés ("Elevate", "Seamless", "Unleash", "Next-Gen").
- No filler UI ("Scroll to explore", bouncing chevrons), no generic names (Acme, John Doe),
  no `LABEL // YEAR` typography, no broken image links.
