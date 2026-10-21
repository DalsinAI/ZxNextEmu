// AudioWorklet: plays the stereo frames the runtime sends, in order, and
// reports how many it still holds so the runtime can pace itself.
class ZxSound extends AudioWorkletProcessor {
  constructor() {
    super();
    this.queue = [];
    this.pos = 0;
    this.port.onmessage = e => {
      this.queue.push(e.data);
      if (this.queue.length > 12) this.queue.splice(0, this.queue.length - 6);
      this.port.postMessage(this.queue.length);
    };
  }
  process(inputs, outputs) {
    const [l, r] = outputs[0];
    let i = 0;
    while (i < l.length && this.queue.length) {
      const f = this.queue[0];
      while (i < l.length && this.pos < f.length) {
        l[i] = f[this.pos];
        if (r) r[i] = f[this.pos + 1];
        this.pos += 2;
        i++;
      }
      if (this.pos >= f.length) {
        this.queue.shift();
        this.pos = 0;
        this.port.postMessage(this.queue.length);
      }
    }
    for (; i < l.length; i++) { l[i] = 0; if (r) r[i] = 0; }
    return true;
  }
}
registerProcessor('zx-sound', ZxSound);
