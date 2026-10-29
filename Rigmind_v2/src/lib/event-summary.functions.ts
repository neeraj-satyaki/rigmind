import { createServerFn } from "@tanstack/react-start";
import { z } from "zod";

const input = z.object({
  eventId: z.string().max(40),
  context: z.string().max(8000),
});

const schema = z.object({
  summary: z.string(),
  recordedFacts: z.array(z.string()),
  missingInformation: z.array(z.string()),
});

export type EventSummaryDraft = z.infer<typeof schema>;

export const draftEventSummary = createServerFn({ method: "POST" })
  .inputValidator((d: unknown) => input.parse(d))
  .handler(async ({ data }): Promise<{ draft?: EventSummaryDraft; error?: string }> => {
    const apiKey = process.env["LOVABLE_API_KEY"];
    if (!apiKey) return { error: "AI summaries are not configured." };
    const { createOpenAI } = await import("@ai-sdk/openai");
    const { streamText, Output } = await import("ai");
    const provider = createOpenAI({
      baseURL: "https://ai.gateway.lovable.dev/v1",
      apiKey,
      headers: { "Lovable-API-Key": apiKey, "X-Lovable-AIG-SDK": "vercel-ai-sdk" },
    });
    try {
      const result = streamText({
        model: provider.responses("openai/gpt-6-astra"),
        maxRetries: 0,
        system:
          "You draft rig safety event summaries for HSE review. Use ONLY the supplied record. Never infer causes, names, durations or outcomes that are not stated. recordedFacts: each item a fact directly stated in the record, citing its source (timeline, evidence, notes, history). missingInformation: what an HSE reviewer would need but is absent or unconfirmed. summary: 2-3 neutral sentences.",
        messages: [{ role: "user", content: `Event ${data.eventId}\n\n${data.context}` }],
        output: Output.object({ schema }),
        providerOptions: {
          openai: {
            store: false,
            forceReasoning: true,
            reasoningEffort: "low",
            reasoningSummary: "auto",
            include: ["reasoning.encrypted_content"],
          },
        },
      });
      const draft = await result.output;
      return { draft };
    } catch (e: unknown) {
      const status = (e as { statusCode?: number }).statusCode;
      console.error("draftEventSummary failed", status, e);
      if (status === 402) return { error: "AI credits are exhausted. Add credits in workspace settings." };
      if (status === 429) return { error: "Too many requests. Please try again shortly." };
      return { error: "The draft summary could not be generated." };
    }
  });
