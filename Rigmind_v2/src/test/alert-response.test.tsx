import { fireEvent, render, screen, waitFor } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import { AppState, useAppState } from '@/components/rig/AppState';
vi.mock('@/lib/device-db',()=>({readDevice:vi.fn(async()=>undefined),writeDevice:vi.fn(async()=>{})}));
function Response(){const {items,respond}=useAppState();return <><span data-testid="status">{items.find(a=>a.id==='EV-1042')?.status}</span><button onClick={()=>respond('EV-1042','Acknowledged')}>Respond</button><button onClick={()=>respond('EV-1042','Escalated')}>Escalate</button></>;}
describe('Alert response',()=>{
 it('acknowledges the selected event',async()=>{render(<AppState><Response/></AppState>);fireEvent.click(screen.getByText('Respond'));await waitFor(()=>expect(screen.getByTestId('status')).toHaveTextContent('Acknowledged'));});
});
