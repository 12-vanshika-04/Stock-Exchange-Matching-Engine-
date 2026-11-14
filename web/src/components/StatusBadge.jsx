import { RefreshCw, ShieldAlert, Wifi, WifiOff } from 'lucide-react';

const VIEW = {
  connecting: { Icon: RefreshCw, label: 'Connecting', className: 'badge badge-wait' },
  connected: { Icon: Wifi, label: 'Live', className: 'badge badge-ok' },
  disconnected: { Icon: WifiOff, label: 'Offline, retrying', className: 'badge badge-bad' },
  refused: { Icon: ShieldAlert, label: 'Login refused', className: 'badge badge-bad' },
};

export default function StatusBadge({ status }) {
  const { Icon, label, className } = VIEW[status] ?? VIEW.disconnected;
  return (
    <span className={className} role="status">
      <Icon size={14} aria-hidden="true" className={status === 'connecting' ? 'spin' : undefined} />
      {label}
    </span>
  );
}