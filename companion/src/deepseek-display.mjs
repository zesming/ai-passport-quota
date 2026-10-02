export function deepSeekRmbDisplay(balance) {
  const info = Array.isArray(balance?.balance_infos)
    ? balance.balance_infos.find(item => item?.currency === 'CNY') ?? null
    : null;
  if (!info) return { info: null, availability: '尚无人民币余额', tone: 'unknown' };
  return balance.is_available === true
    ? { info, availability: '余额可用', tone: 'available' }
    : { info, availability: '暂不可用', tone: 'unavailable' };
}
