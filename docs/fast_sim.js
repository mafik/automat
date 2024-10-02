let canvas = document.getElementById("canvas");
let ctx = canvas.getContext("2d");
var width = window.innerWidth;
var height = window.innerHeight;

function OnResize() {
  width = window.innerWidth;
  height = window.innerHeight;
  let density = window.devicePixelRatio || 1;
  canvas.width = window.innerWidth * density;
  canvas.height = window.innerHeight * density;
  ctx = canvas.getContext("2d");
  ctx.font = "20px Iosevka";
  ctx.scale(density, density);
}

window.addEventListener("resize", OnResize);
OnResize();

let pts = [];

for (let i = 0; i < 100; i++) {
  // 100 points at random locations
  let p = {
    now: {
      x: Math.random() * width,
      y: Math.random() * height
    },

  };
  p.last = {
    x: p.now.x + Math.random() * 1e-1,
    y: p.now.y + Math.random() * 1e-1
  };
  pts.push(p);
}

function AnimationFrame() {
  requestAnimationFrame(AnimationFrame);

  for (let p1 of pts) {
    for (let p2 of pts) {
      let dx = p2.now.x - p1.now.x;
      let dy = p2.now.y - p1.now.y;
      let d = Math.sqrt(dx * dx + dy * dy);
      // p1.now.x += dx / 1000;
      // p1.now.y += dy / 1000;
    }
  }

  for (let p of pts) {
    let nx = p.now.x;
    let ny = p.now.y;
    p.now.x += p.now.x - p.last.x;
    p.now.y += p.now.y - p.last.y;
    p.last.x = nx;
    p.last.y = ny;
  }

  ctx.clearRect(0, 0, width, height);
  for (let p of pts) {
    ctx.beginPath();
    ctx.arc(p.now.x, p.now.y, 2, 0, 2 * Math.PI);
    ctx.fill();
  }

}

AnimationFrame(0);


