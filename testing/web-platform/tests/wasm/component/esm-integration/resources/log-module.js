export default function log(message) {
  (globalThis.logged ??= []).push(message);
}
