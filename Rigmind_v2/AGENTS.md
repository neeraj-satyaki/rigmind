<!-- LOVABLE:BEGIN -->
> [!IMPORTANT]
> This project is connected to [Lovable](https://lovable.dev). Avoid rewriting
> published git history — force pushing, or rebasing/amending/squashing commits
> that are already pushed — as it rewrites history on Lovable's side and the
> user will likely lose their project history.
>
> Commits you push to the connected branch sync back to Lovable and show up in
> the editor, so keep the branch in a working state.
<!-- LOVABLE:END -->

## RigMind architecture
- Use TanStack file routes for every screen; this is the supported router and preserves the existing URLs.
- Keep all demonstration fixtures and policy values in `src/data/mock.ts` so API replacement stays localized.
- Group related screen implementations in focused `src/components/rig` modules, with thin route wrappers, to share presentation without coupling route modules.
- Keep authentication and physical outputs simulated; persist camera registrations, theme preferences and alert responses in browser-local SQLite backed by IndexedDB, because the user selected per-device storage.
- Initialize sql.js only after hydration or in event handlers; load its WASM from the asset pointer to avoid server-side browser access.
- Printable reports live in `src/reports`: scoped `reports.css` under a `.rm-report` root, shared components in `src/reports/components.tsx`, one template per report in `src/reports/templates/`, and the `/report/$reportId` route renders them outside the app shell with a print toolbar; report values stay in `src/data/mock.ts` so templates remain data-driven.
- Keep startup sequencing in useBootSequence and page transitions in usePageTransition with client-effect timers and router subscriptions; this preserves SSR safety and the persistent shell while allowing real startup tasks to replace timers later.
- AI event summaries run only in a server function (`src/lib/event-summary.functions.ts`) via Lovable AI Gateway, so the API key stays server-side; drafts are never report-ready until HSE approval.
- Keep the login monitoring canvas in its own client-mounted component with effect-owned drawing and observers, so SSR remains browser-independent and hidden visuals consume no animation work.
