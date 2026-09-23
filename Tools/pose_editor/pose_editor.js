const bones = [
  ["pelvis", -1, [0, 0.92, 0]],
  ["chest", 0, [0, 0.36, 0]],
  ["head", 1, [0, 0.34, 0]],
  ["face", 1, [0, 0.40, 0.10]],
  ["left arm", 1, [-0.24, 0.22, 0]],
  ["left forearm", 4, [-0.34, -0.02, 0]],
  ["left hand", 5, [-0.30, -0.02, 0]],
  ["right arm", 1, [0.24, 0.22, 0]],
  ["right forearm", 7, [0.34, -0.02, 0]],
  ["right hand", 8, [0.30, -0.02, 0]],
  ["left thigh", 0, [-0.13, -0.42, 0]],
  ["left shin", 10, [0, -0.45, 0]],
  ["left foot", 11, [0, -0.12, 0.13]],
  ["right thigh", 0, [0.13, -0.42, 0]],
  ["right shin", 13, [0, -0.45, 0]],
  ["right foot", 14, [0, -0.12, 0.13]]
];

const deg = Math.PI / 180;
const slots = {
  idle: makePose(),
  stab: makePose()
};
let activeSlot = "idle";
let selected = 9;
let yaw = 0.55;
let pitch = 0.18;
let zoom = 2.9;
let dragging = false;
let lastMouse = [0, 0];

function makePose() {
  return {
    bone: Array.from({ length: 16 }, () => ({ r: [0, 0, 0], p: [0, 0, 0] })),
    item: { bone: 9, p: [0.02, -0.03, 0.22], r: [0, 0, 0] }
  };
}

function clonePose(pose) {
  return JSON.parse(JSON.stringify(pose));
}

function pose() {
  return slots[activeSlot];
}

function matIdentity() {
  return [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1];
}
function matMul(a, b) {
  const r = new Array(16).fill(0);
  for (let c = 0; c < 4; c++) for (let y = 0; y < 4; y++) {
    for (let k = 0; k < 4; k++) r[c*4+y] += a[k*4+y] * b[c*4+k];
  }
  return r;
}
function translate(v) {
  const m = matIdentity();
  m[12] = v[0]; m[13] = v[1]; m[14] = v[2];
  return m;
}
function rotX(a) {
  const c = Math.cos(a), s = Math.sin(a), m = matIdentity();
  m[5] = c; m[6] = s; m[9] = -s; m[10] = c;
  return m;
}
function rotY(a) {
  const c = Math.cos(a), s = Math.sin(a), m = matIdentity();
  m[0] = c; m[2] = -s; m[8] = s; m[10] = c;
  return m;
}
function rotZ(a) {
  const c = Math.cos(a), s = Math.sin(a), m = matIdentity();
  m[0] = c; m[1] = s; m[4] = -s; m[5] = c;
  return m;
}
function transform(m, v) {
  return [
    m[0]*v[0] + m[4]*v[1] + m[8]*v[2] + m[12],
    m[1]*v[0] + m[5]*v[1] + m[9]*v[2] + m[13],
    m[2]*v[0] + m[6]*v[1] + m[10]*v[2] + m[14]
  ];
}
function add(a, b) {
  return [a[0]+b[0], a[1]+b[1], a[2]+b[2]];
}

function computeWorld(which = pose()) {
  const world = [];
  for (let i = 0; i < bones.length; i++) {
    const b = which.bone[i];
    const local = matMul(
      matMul(matMul(rotZ(b.r[2] * deg), rotY(b.r[1] * deg)), rotX(b.r[0] * deg)),
      translate(add(bones[i][2], b.p))
    );
    const parent = bones[i][1];
    world[i] = parent < 0 ? local : matMul(world[parent], local);
  }
  return world;
}

function bonePositions(which = pose()) {
  return computeWorld(which).map(m => transform(m, [0, 0, 0]));
}

function project(v, w, h) {
  let x = v[0], y = v[1] - 0.55, z = v[2];
  const cy = Math.cos(yaw), sy = Math.sin(yaw);
  const cp = Math.cos(pitch), sp = Math.sin(pitch);
  let x1 = x * cy - z * sy;
  let z1 = x * sy + z * cy;
  let y1 = y * cp - z1 * sp;
  z1 = y * sp + z1 * cp + zoom;
  const s = Math.min(w, h) * 0.78 / Math.max(0.2, z1);
  return [w / 2 + x1 * s, h * 0.54 - y1 * s];
}

function draw() {
  const c = document.getElementById("view");
  const dpr = window.devicePixelRatio || 1;
  const rect = c.getBoundingClientRect();
  c.width = Math.max(1, Math.floor(rect.width * dpr));
  c.height = Math.max(1, Math.floor(rect.height * dpr));
  const ctx = c.getContext("2d");
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, rect.width, rect.height);
  const pos = bonePositions();
  const pts = pos.map(p => project(p, rect.width, rect.height));
  ctx.lineCap = "round";
  ctx.lineJoin = "round";
  for (let i = 0; i < bones.length; i++) {
    const parent = bones[i][1];
    if (parent < 0) continue;
    ctx.strokeStyle = i === selected || parent === selected ? "#8bd3ff" : "#d7e2eb";
    ctx.lineWidth = i === selected || parent === selected ? 6 : 4;
    ctx.beginPath();
    ctx.moveTo(pts[parent][0], pts[parent][1]);
    ctx.lineTo(pts[i][0], pts[i][1]);
    ctx.stroke();
  }
  for (let i = 0; i < bones.length; i++) {
    ctx.fillStyle = i === selected ? "#ffd36b" : "#1b2732";
    ctx.strokeStyle = "#eef6ff";
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.arc(pts[i][0], pts[i][1], i === selected ? 8 : 5, 0, Math.PI * 2);
    ctx.fill();
    ctx.stroke();
  }
  const item = pose().item;
  const attach = pts[item.bone] || pts[9];
  ctx.strokeStyle = "#ff5f6f";
  ctx.lineWidth = 5;
  ctx.beginPath();
  ctx.moveTo(attach[0], attach[1]);
  ctx.lineTo(attach[0] + 42 + item.p[2] * 120, attach[1] - 12 - item.p[1] * 80);
  ctx.stroke();
  requestAnimationFrame(draw);
}

function buildBoneList() {
  const list = document.getElementById("bones");
  list.innerHTML = "";
  bones.forEach((b, i) => {
    const btn = document.createElement("button");
    btn.className = "bone" + (i === selected ? " active" : "");
    const indent = depthOf(i) * 14;
    btn.innerHTML = `<span style="padding-left:${indent}px"><span class="idx">${i}</span>${b[0]}</span>`;
    btn.onclick = () => {
      selected = i;
      syncFields();
      buildBoneList();
    };
    list.appendChild(btn);
  });
}
function depthOf(i) {
  let d = 0, p = bones[i][1];
  while (p >= 0) { d++; p = bones[p][1]; }
  return d;
}

function syncFields() {
  document.getElementById("selectedTitle").textContent = `${selected}: ${bones[selected][0]}`;
  const b = pose().bone[selected];
  ["rx","ry","rz"].forEach((id, i) => document.getElementById(id).value = round(b.r[i], 3));
  ["px","py","pz"].forEach((id, i) => document.getElementById(id).value = round(b.p[i], 4));
  const item = pose().item;
  document.getElementById("itemBone").value = item.bone;
  ["itemX","itemY","itemZ"].forEach((id, i) => document.getElementById(id).value = round(item.p[i], 4));
  ["itemRX","itemRY","itemRZ"].forEach((id, i) => document.getElementById(id).value = round(item.r[i], 3));
}
function readFields() {
  const b = pose().bone[selected];
  ["rx","ry","rz"].forEach((id, i) => b.r[i] = num(id));
  ["px","py","pz"].forEach((id, i) => b.p[i] = num(id));
  const item = pose().item;
  item.bone = Math.max(0, Math.min(15, Math.round(num("itemBone"))));
  ["itemX","itemY","itemZ"].forEach((id, i) => item.p[i] = num(id));
  ["itemRX","itemRY","itemRZ"].forEach((id, i) => item.r[i] = num(id));
}
function num(id) {
  const v = Number(document.getElementById(id).value);
  return Number.isFinite(v) ? v : 0;
}
function round(v, places) {
  return Number(v.toFixed(places));
}

function mirrorSelected() {
  const pairs = {4:7,5:8,6:9,7:4,8:5,9:6,10:13,11:14,12:15,13:10,14:11,15:12};
  const other = pairs[selected];
  if (other == null) return;
  const src = pose().bone[selected];
  const dst = pose().bone[other];
  dst.r = [src.r[0], -src.r[1], -src.r[2]];
  dst.p = [-src.p[0], src.p[1], src.p[2]];
  selected = other;
  syncFields();
  buildBoneList();
}

function applyKnifeIdle() {
  activeSlot = document.getElementById("slot").value = "idle";
  const p = pose();
  p.bone[7].r = [18, 0, -18];
  p.bone[8].r = [-28, 8, -12];
  p.bone[9].r = [0, 10, 0];
  p.item = { bone: 9, p: [0.02, -0.03, 0.22], r: [0, 0, 0] };
  syncFields();
}

function applyKnifeStab() {
  activeSlot = document.getElementById("slot").value = "stab";
  slots.stab = clonePose(slots.idle);
  const p = pose();
  p.bone[7].r = [5, -6, -6];
  p.bone[8].r = [-8, 3, -5];
  p.bone[9].r = [0, 2, 0];
  p.item = { bone: 9, p: [-0.01, 0.01, 0.46], r: [0, 0, 0] };
  syncFields();
}

function exportCpp() {
  readFields();
  let s = "";
  for (const name of ["idle", "stab"]) {
    s += `// ${name}\n`;
    s += `static constexpr float ${name}BoneRotDeg[16][3] = {\n`;
    for (const b of slots[name].bone) s += `    {${fmt(b.r[0])}f, ${fmt(b.r[1])}f, ${fmt(b.r[2])}f},\n`;
    s += `};\n`;
    s += `static constexpr float ${name}BonePos[16][3] = {\n`;
    for (const b of slots[name].bone) s += `    {${fmt(b.p[0])}f, ${fmt(b.p[1])}f, ${fmt(b.p[2])}f},\n`;
    s += `};\n`;
    const item = slots[name].item;
    s += `static constexpr int ${name}ItemBone = ${item.bone};\n`;
    s += `static constexpr float ${name}ItemPos[3] = {${fmt(item.p[0])}f, ${fmt(item.p[1])}f, ${fmt(item.p[2])}f};\n`;
    s += `static constexpr float ${name}ItemRotDeg[3] = {${fmt(item.r[0])}f, ${fmt(item.r[1])}f, ${fmt(item.r[2])}f};\n\n`;
  }
  document.getElementById("out").value = s;
}

function exportLua() {
  readFields();
  let s = "return {\n";
  for (const name of ["idle", "stab"]) {
    s += `  ${name} = {\n    bones = {\n`;
    slots[name].bone.forEach((b, i) => {
      s += `      [${i}] = { rot = { ${fmt(b.r[0])}, ${fmt(b.r[1])}, ${fmt(b.r[2])} }, pos = { ${fmt(b.p[0])}, ${fmt(b.p[1])}, ${fmt(b.p[2])} } },\n`;
    });
    const item = slots[name].item;
    s += `    },\n    item = { bone = ${item.bone}, pos = { ${fmt(item.p[0])}, ${fmt(item.p[1])}, ${fmt(item.p[2])} }, rot = { ${fmt(item.r[0])}, ${fmt(item.r[1])}, ${fmt(item.r[2])} } }\n  },\n`;
  }
  s += "}\n";
  document.getElementById("out").value = s;
}

function fmt(v) {
  let text = (Math.round(v * 10000) / 10000).toFixed(4);
  text = text.replace(/0+$/, "").replace(/\.$/, "");
  if (!text.includes(".")) text += ".0";
  return text;
}

function saveJson() {
  readFields();
  const blob = new Blob([JSON.stringify({ version: 1, slots }, null, 2)], { type: "application/json" });
  const a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  a.download = "sandium_pose.json";
  a.click();
  URL.revokeObjectURL(a.href);
}

function setup() {
  buildBoneList();
  syncFields();
  for (const id of ["rx","ry","rz","px","py","pz","itemBone","itemX","itemY","itemZ","itemRX","itemRY","itemRZ"]) {
    document.getElementById(id).addEventListener("input", readFields);
  }
  document.getElementById("slot").onchange = e => { readFields(); activeSlot = e.target.value; syncFields(); };
  document.getElementById("zeroBone").onclick = () => { pose().bone[selected] = { r: [0,0,0], p: [0,0,0] }; syncFields(); };
  document.getElementById("zeroAll").onclick = () => { slots[activeSlot] = makePose(); syncFields(); };
  document.getElementById("mirror").onclick = mirrorSelected;
  document.getElementById("copyIdleToStab").onclick = () => { slots.stab = clonePose(slots.idle); activeSlot = document.getElementById("slot").value = "stab"; syncFields(); };
  document.getElementById("knifePreset").onclick = applyKnifeIdle;
  document.getElementById("stabPreset").onclick = applyKnifeStab;
  document.getElementById("exportCpp").onclick = exportCpp;
  document.getElementById("exportLua").onclick = exportLua;
  document.getElementById("saveJson").onclick = saveJson;
  document.getElementById("loadJson").onclick = () => document.getElementById("fileInput").click();
  document.getElementById("fileInput").onchange = async e => {
    const file = e.target.files[0];
    if (!file) return;
    const data = JSON.parse(await file.text());
    if (data.slots) {
      slots.idle = data.slots.idle || slots.idle;
      slots.stab = data.slots.stab || slots.stab;
      syncFields();
    }
  };
  const canvas = document.getElementById("view");
  canvas.onmousedown = e => { dragging = true; lastMouse = [e.clientX, e.clientY]; };
  window.onmouseup = () => dragging = false;
  window.onmousemove = e => {
    if (!dragging) return;
    yaw += (e.clientX - lastMouse[0]) * 0.008;
    pitch = Math.max(-1.1, Math.min(1.1, pitch + (e.clientY - lastMouse[1]) * 0.006));
    lastMouse = [e.clientX, e.clientY];
  };
  canvas.onwheel = e => {
    e.preventDefault();
    zoom = Math.max(1.2, Math.min(6.0, zoom + e.deltaY * 0.002));
  };
  applyKnifeIdle();
  slots.stab = clonePose(slots.idle);
  applyKnifeStab();
  activeSlot = document.getElementById("slot").value = "idle";
  syncFields();
  draw();
}

setup();
