/** Encode a string as the component BOF format: 2-byte LE length + UTF-8 + NUL. */
export function encodeBeaconString(value: string): number[] {
  const bytes = Array.from(new TextEncoder().encode(value))
  const len = bytes.length + 1
  return [len & 0xff, (len >> 8) & 0xff, ...bytes, 0]
}

/** Encode raw text as UTF-8 bytes without any length prefix. */
export function encodeRawText(value: string): number[] {
  return Array.from(new TextEncoder().encode(value))
}

/** Encode a hex string into raw bytes. Whitespace is ignored. */
export function encodeRawHex(hex: string): number[] {
  const cleaned = hex.replace(/\s+/g, '')
  const bytes: number[] = []
  for (let i = 0; i < cleaned.length; i += 2) {
    const b = parseInt(cleaned.substring(i, i + 2), 16)
    if (Number.isNaN(b)) throw new Error(`Invalid hex at position ${i}: "${cleaned.substring(i, i + 2)}"`)
    bytes.push(b)
  }
  return bytes
}
