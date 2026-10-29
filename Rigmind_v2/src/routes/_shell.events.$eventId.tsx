import { createFileRoute } from "@tanstack/react-router";
import { pageHead } from "@/components/rig/ui";
import { EventInvestigation } from "@/components/rig/ReviewScreens";

export const Route = createFileRoute("/_shell/events/$eventId")({
  head: pageHead("Event investigation", "Event investigation in RigMind safety operations."),
  component: EventPage,
});

function EventPage() {
  const { eventId } = Route.useParams();
  return <EventInvestigation key={eventId} eventId={eventId} />;
}
