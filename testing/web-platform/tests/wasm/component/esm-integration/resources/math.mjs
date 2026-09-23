export function add(a, b) {
  return a + b;
}

export class Thing {
  constructor(value) {
    this.value = value;
  }
  get() {
    return this.value;
  }
}
