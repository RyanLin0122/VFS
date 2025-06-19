class CacheNode {
  constructor() {
    this.lca = -1;
    this.ready = false;
  }
}

class CacheSim {
  constructor(params) {
    this.params = params;
    this.cacheNum = params.cacheNum;
    this.bucketNum = params.bucketNum;
    this.nodes = new Array(this.cacheNum).fill(0).map(() => new CacheNode());
    this.buckets = new Array(this.bucketNum).fill(0).map(() => []);
    this.freeList = [];
    for (let i = 0; i < this.cacheNum; i++) this.freeList.push(i);
    this.lru = [];
    this.hit = 0;
    this.miss = 0;
  }

  hash(lca) {
    // simple hash
    return (lca * 2654435761) >>> 0;
  }

  getBucket(lca) {
    return this.hash(lca) % this.bucketNum;
  }

  moveToRecent(idx) {
    const pos = this.lru.indexOf(idx);
    if (pos !== -1) this.lru.splice(pos, 1);
    this.lru.push(idx);
  }

  evictLRU() {
    const idx = this.lru.shift();
    if (idx === undefined) return null;
    const node = this.nodes[idx];
    if (node.lca !== -1) {
      const b = this.getBucket(node.lca);
      const arr = this.buckets[b];
      const p = arr.indexOf(idx);
      if (p !== -1) arr.splice(p, 1);
    }
    node.lca = -1;
    node.ready = false;
    this.freeList.push(idx);
    return idx;
  }

  getNode(lca) {
    const b = this.getBucket(lca);
    const arr = this.buckets[b];
    for (let i = 0; i < arr.length; i++) {
      const idx = arr[i];
      if (this.nodes[idx].lca === lca) return idx;
    }
    return null;
  }

  allocateNode(lca) {
    let idx = this.freeList.pop();
    if (idx === undefined) {
      idx = this.evictLRU();
    }
    const node = this.nodes[idx];
    node.lca = lca;
    node.ready = true;
    this.moveToRecent(idx);
    const b = this.getBucket(lca);
    this.buckets[b].push(idx);
    return idx;
  }

  read(lca) {
    const idx = this.getNode(lca);
    if (idx !== null) {
      this.hit++;
      this.moveToRecent(idx);
    } else {
      this.miss++;
      this.allocateNode(lca);
    }
  }

  write(lca) {
    const idx = this.getNode(lca);
    if (idx !== null) {
      // evict existing
      const b = this.getBucket(lca);
      const arr = this.buckets[b];
      const p = arr.indexOf(idx);
      if (p !== -1) arr.splice(p, 1);
      const pos = this.lru.indexOf(idx);
      if (pos !== -1) this.lru.splice(pos, 1);
      this.freeList.push(idx);
    }
    this.allocateNode(lca);
  }
}

function getParams() {
  return {
    pattern: document.getElementById('pattern').value,
    cmdSize: parseInt(document.getElementById('cmdSize').value),
    cmdNum: parseInt(document.getElementById('cmdNum').value),
    lcaRange: parseInt(document.getElementById('lcaRange').value),
    cacheNum: parseInt(document.getElementById('cacheNum').value),
    bucketNum: parseInt(document.getElementById('bucketNum').value),
    bufferNum: parseInt(document.getElementById('bufferNum').value),
    nandRead: parseInt(document.getElementById('nandRead').value),
    nandProg: parseInt(document.getElementById('nandProg').value),
    progCnt: parseInt(document.getElementById('progCnt').value),
    queueDepth: parseInt(document.getElementById('queueDepth').value)
  };
}

function randomLCA(range) {
  return Math.floor(Math.random() * range);
}

function runPattern() {
  const params = getParams();
  const sim = new CacheSim(params);
  let lca = 0;
  for (let i = 0; i < params.cmdNum; i++) {
    let cmdLCA;
    if (params.pattern === 'seqRead' || params.pattern === 'seqWrite') {
      cmdLCA = lca;
      lca += params.cmdSize;
      if (lca >= params.lcaRange) lca = 0;
    } else {
      cmdLCA = randomLCA(params.lcaRange);
    }
    if (params.pattern === 'seqRead' || params.pattern === 'randRead') {
      sim.read(cmdLCA);
    } else {
      sim.write(cmdLCA);
    }
  }
  const hitRate = sim.hit / params.cmdNum;
  const missRate = sim.miss / params.cmdNum;
  document.getElementById('result').textContent =
    `Hit Rate: ${(hitRate*100).toFixed(2)}%  Miss Rate: ${(missRate*100).toFixed(2)}%`;
}

function setupUI() {
  const btn = document.getElementById('startBtn');
  btn.addEventListener('click', () => {
    document.getElementById('result').textContent = '';
    document.getElementById('status').innerHTML = '<div class="spinner"></div>';
    setTimeout(() => {
      runPattern();
      document.getElementById('status').innerHTML = '';
    }, 50);
  });
}

document.addEventListener('DOMContentLoaded', setupUI);

let simCanvas;
function setup() {
  simCanvas = createCanvas(760, 200);
  simCanvas.parent('visualization');
}

function draw() {
  background(240);
  fill('#3498db');
  noStroke();
  const params = getParams();
  const count = params.cacheNum;
  const size = width / count;
  for (let i = 0; i < count; i++) {
    rect(i * size, 50, size - 1, 50);
  }
}
