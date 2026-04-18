// EventLog.jsx
// 발행된 이벤트 목록 (최근 30건)

const PRIORITY_CLASS = {
  HIGH:   'badge--red',
  MEDIUM: 'badge--orange',
  LOW:    'badge--gray',
};

const TYPE_LABEL = {
  MOTION:     '모션',
  DOOR_FORCED:'강제 개문',
  INTRUSION:  '침입',
};

function timeStr(sentAt) {
  return new Date(sentAt).toLocaleTimeString('ko-KR', { hour12: false });
}

export function EventLog({ events }) {
  if (events.length === 0) {
    return (
      <div className="event-log event-log--empty">
        발행된 이벤트가 없습니다
      </div>
    );
  }

  return (
    <ol className="event-log">
      {events.map((ev) => (
        <li key={ev.msg_id} className="event-log__item">
          <span className="event-log__time">{timeStr(ev.sentAt)}</span>
          <span className={`badge ${PRIORITY_CLASS[ev.priority] ?? 'badge--gray'}`}>
            {TYPE_LABEL[ev.type] ?? ev.type}
          </span>
          <span className="event-log__topic">{ev.topic}</span>
        </li>
      ))}
    </ol>
  );
}
