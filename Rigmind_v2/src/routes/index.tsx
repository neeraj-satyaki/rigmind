import { createFileRoute, redirect } from "@tanstack/react-router";

export const Route = createFileRoute("/")({
  beforeLoad: () => {
    throw redirect({ to: "/login" });
  },
  head: () => ({
    meta: [
      { title: "RigMind · Rig safety operations" },
      { name: "description", content: "Monitor coverage, investigate events and coordinate the shift response." },
      { property: "og:title", content: "RigMind · Rig safety operations" },
      { property: "og:description", content: "Monitor coverage, investigate events and coordinate the shift response." },
      { property: "og:type", content: "website" },
      { name: "twitter:card", content: "summary" },
    ],
  }),
});
