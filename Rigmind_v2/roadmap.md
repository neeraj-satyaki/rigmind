# RigMind remaining screens
- [x] Event history, investigation and evidence library
- [x] Safety analytics and event overview
- [x] Safety rules, zone editor, health, license and integrations
- [x] Actions, users, audit and reports
- [x] Verify navigation and key mock interactions
- [x] Replace APP LOGO placeholders with the uploaded logo and matching favicon
- [x] Show the full logo with the RigMind name

## Camera and navigation updates
- [x] Make stream validation and zone assignment steps interactive
- [x] Bell event dropdown and toast notifications
- [x] Alert notifications — stored in the on-device SQLite database and delivered by the desktop host (toast, bell, tone, tray); no cloud push service
- [x] Dark theme and navigation search suggestions
- [x] Detailed mock user profile and logout
- [x] Satyaki wordmark, requested footer, remove DEMO and Safety workspace
- [x] Verify camera and navigation flows
- [x] Fix remaining inactive page buttons and tabs, especially Alert inbox
- [x] Add a RigMind loader using a generated image
- [x] Per-device SQLite storage for camera details, theme and alert responses
- [x] Satyaki wordmark and branding on login
- [x] Import official Satyaki logo from satyaki.co.in and show it in the app and login

## Supplied loader integration
- [x] Add supplied RigMind mark, boot splash and page loader designs
- [x] Startup sequence, 700ms page transitions and top progress; retain TanStack automatic screen splitting
- [x] Use the supplied mark with the RigMind name in sidebar and login
- [x] Verify startup, navigation and loader timing
- [x] Weekly PDF report like W43 sample, with HSE-approved summaries only
- [x] Persist drafts + HSE decisions in SQLite
- [x] Editable profile email
- [x] Thin app footer
- [x] Logo animation visible in dark theme
- [x] Per alert: animate logo, then toast + bell count

## Printable report system
- [x] 10 report templates matching the reference design, shared components and scoped CSS
- [x] Report viewer route /report/$reportId with print toolbar and CSV downloads
- [x] Wire every report CTA across Reports, Events, Audit, Actions, Fleet, Analytics, Health
- [x] ™ superscript on the RigMind wordmark (sidebar, login, boot splash, report headers)
- [x] Verify every report route and CTA in the browser

## Nav bar and branding tweaks
- [x] ™ superscript on the RigMind wordmark (sidebar, login, boot splash, report header)
- [x] Nav bar order: search, bell, theme, avatar
- [x] Toasts at the top on phones (below 768px), unchanged bottom-right on desktop
- [x] Login: hide the rig photo on mobile only (below 768px), desktop unchanged
- [x] Login mobile bar: drop the Enterprise access button and Powered by line (desktop unchanged)
- [x] Sign-in card nearer the top on mobile, small gap under the thin navbar
- [x] Enterprise SSO button asks for an email to add, saved on this device
- [x] Login mobile bar: small gap between the theme toggle and the Satyaki logo
- [x] ™ removed from the login wordmark and hidden on every phone-sized screen
- [x] Satyaki wordmark set in Poppins to match satyaki.co.in (app bar and report header)
- [x] Login (desktop): align the Satyaki logo + "Powered by Satyaki Solutions © 2026" block flush right
- [x] Login: hide the rig photo and marketing text on tablet widths too (show only on large desktop)
- [x] Login: fix the "Powered by Satyaki Solutions © 2026" placement
- [x] Satyaki wordmark in its own brand colours (maroon and gold) in app bar, login and reports

## Login branding tidy-up
- [x] Satyaki wordmark ("SATYAKI SOLUTIONS") on one line
- [x] Remove the "Enterprise access" button from every screen
- [x] Remove "Powered by Satyaki Solutions © 2026" from the login page at all screen sizes
- [x] Replace the login photo with a theme-aware interactive monitoring canvas; verify pointer, motion and viewport behavior
- [x] Login: remove the left marketing section, centre the sign-in card, keep the RigMind logo top-left
- [x] Login: enlarge the top-left RigMind mark and wordmark
- [x] Login: keep the top-left RigMind mark static (no animation)
