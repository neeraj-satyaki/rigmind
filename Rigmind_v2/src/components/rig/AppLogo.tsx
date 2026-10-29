import logo from "@/assets/rigmind-wordmark.png.asset.json";
import { cn } from "@/lib/utils";

export function AppLogo({ className }: { className?: string }) {
  return <img src={logo.url} alt="RigMind" width={912} height={315} className={cn("h-auto w-40 object-contain", className)} />;
}