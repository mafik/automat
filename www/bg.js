
let canvas = document.getElementById('bg');
let ctx = canvas.getContext('2d');

window.addEventListener('resize', () => {
  canvas.width = window.innerWidth;
  canvas.height = window.innerHeight;
});

canvas.width = window.innerWidth;
canvas.height = window.innerHeight;

let points = [];
let history = [];

const n_points = 15;
for (let i = 0; i < n_points; i++) {
  let angle = Math.random() * Math.PI * 2;
  let vscale = 50;
  let rscale = 100;
  let omega_scale = 1;
  let canvas_center = { x: canvas.width / 2, y: canvas.height / 2 };
  let alpha = Math.PI * 2 / n_points * i;
  points.push({
    x: canvas_center.x + Math.cos(alpha) * canvas.width / 4 + Math.random() * canvas.width / 8,
    y: canvas_center.y + Math.sin(alpha) * canvas.height / 4 + Math.random() * canvas.height / 8,
    vx: Math.cos(angle) * vscale,
    vy: Math.sin(angle) * vscale,
    radius: (Math.random() + 0.5) * rscale,
    angle: Math.random() * Math.PI * 2,
    angularVelocity: (Math.random() - 0.5) * omega_scale
  });
}

function MovePoints(pts, step) {
  // Attract points to the center
  for (let p1 of pts) {
    let dx1 = canvas.width / 4 - p1.x;
    let dx2 = canvas.width * 3 / 4 - p1.x;
    let dy = canvas.height / 2 - p1.y;
    let d1 = Math.hypot(dx1, dy);
    let d2 = Math.hypot(dx2, dy);
    let d = Math.min(d1, d2);
    let angle = 0;
    if (d1 < d2) {
      angle = Math.atan2(dy, dx1);
    } else {
      angle = Math.atan2(dy, dx2);
    }
    let f = d;
    p1.vx += Math.cos(angle) * f * step;
    p1.vy += Math.sin(angle) * f * step;
  }
  // Push points away from each other
  for (let p1 of pts) {
    for (let p2 of pts) {
      if (p1 == p2) continue;
      let dx = p2.x - p1.x;
      let dy = p2.y - p1.y;
      let d = Math.hypot(dx, dy);
      if (d < 0.001) d = 0.001;
      // TODO: figure out the constant based on canvas size
      let f = canvas.width * canvas.height / Math.pow(Math.abs(d), 2);
      let angle = Math.atan2(dy, dx);
      p1.vx -= Math.cos(angle) * f * step * canvas.width / canvas.height;
      p1.vy -= Math.sin(angle) * f * step * canvas.height / canvas.width;
    }
  }
  for (let i = 0; i < pts.length; i++) {
    let next_i = (i + 1) % pts.length;
    let prev_i = (i + pts.length - 1) % pts.length;
    let center = {
      x: (pts[next_i].x + pts[prev_i].x) / 2,
      y: (pts[next_i].y + pts[prev_i].y) / 2
    };
    let p = pts[i];
    let dx = center.x - p.x;
    let dy = center.y - p.y;
    let d = Math.sqrt(dx * dx + dy * dy);
    let f = d;
    let angle = Math.atan2(dy, dx);
    p.vx += Math.cos(angle) * f * step;
    p.vy += Math.sin(angle) * f * step;
  }
  // add some friction
  let friction = Math.exp(-1 * step);
  for (let point of pts) {
    point.vx *= friction;
    point.vy *= friction;
    let v = Math.hypot(point.vx, point.vy);
    const vmin = 50;
    if (v < vmin) {
      v = vmin;
      // recalculate vx based on v
      let angle = Math.atan2(point.vy, point.vx);
      point.vx = Math.cos(angle) * v;
      point.vy = Math.sin(angle) * v;
    }
  }
  for (let point of pts) {
    point.x += point.vx * step;
    point.y += point.vy * step;
    point.angle += point.angularVelocity * step;
    if (point.x < 0 || point.x > canvas.width) {
      point.vx *= -1;
      if (point.x < 0) {
        point.x = - point.x;
      } else if (point.x > canvas.width) {
        point.x = canvas.width - (point.x - canvas.width);
      }
    }
    if (point.y < 0 || point.y > canvas.height) {
      point.vy *= -1;
      if (point.y < 0) {
        point.y = point.y;
      } else if (point.y > canvas.height) {
        point.y = canvas.height - (point.y - canvas.height);
      }
    }
  }
}

function Clamp(x, min, max) {
  if (x < min) return min;
  if (x > max) return max;
  return x;
}

let last_t = 0;
function Tick(t) {
  let dt = t - last_t;
  last_t = t;
  dt /= 1000;
  if (dt > 0.05) dt = 0.05;
  MovePoints(points, dt);

  let canvas_scale_factor = Math.sqrt(canvas.width * canvas.height) / 600;

  history.push(JSON.parse(JSON.stringify(points)));

  if (history.length > 1000) {
    history.shift();
  }

  // Draw 10% white to fate the previous frame to white
  ctx.globalCompositeOperation = 'source-over';
  ctx.fillStyle = '#dfddd3';
  ctx.fillRect(0, 0, canvas.width, canvas.height);

  let furthest_d = 1;
  for (let i = 1; i < points.length; i++) {
    let dx = points[0].x - points[i].x;
    let dy = points[0].y - points[i].y;
    furthest_d = Math.max(furthest_d, Math.hypot(dx, dy));
  }

  let gradient = ctx.createRadialGradient(points[0].x, points[0].y, 0, points[0].x, points[0].y, furthest_d * 1.2);
  gradient.addColorStop(0, '#2069c2');
  gradient.addColorStop(1, '#cca916');
  ctx.fillStyle = gradient;
  ctx.globalCompositeOperation = "color-burn";

  const spacing = 10;
  const width = 5;
  const n_strips = 12;
  for (let strip = 0; strip < n_strips; ++strip) {
    let strip_start_i = history.length - strip * spacing - 1;
    let strip_end_i = history.length - strip * spacing - width - 1;

    strip_start_i = Clamp(strip_start_i, 0, history.length - 1);
    strip_end_i = Clamp(strip_end_i, 0, history.length - 1);

    let ribbon_start = history[strip_start_i];

    let opacity = 1 - strip / n_strips;
    ctx.globalAlpha = opacity;


    ctx.beginPath();


    // Draw the bezier array
    ctx.moveTo(ribbon_start[ribbon_start.length - 1].x, ribbon_start[ribbon_start.length - 1].y);
    for (let i = 0; i < ribbon_start.length; i++) {
      let prev_i = (i + ribbon_start.length - 1) % ribbon_start.length;
      let next_i = (i + 1) % ribbon_start.length;

      let cp1 = {
        x: ribbon_start[prev_i].x - Math.cos(ribbon_start[prev_i].angle) * ribbon_start[prev_i].radius * canvas_scale_factor,
        y: ribbon_start[prev_i].y - Math.sin(ribbon_start[prev_i].angle) * ribbon_start[prev_i].radius * canvas_scale_factor
      };
      let p = { x: ribbon_start[i].x, y: ribbon_start[i].y };
      let cp2 = {
        x: ribbon_start[i].x + Math.cos(ribbon_start[i].angle) * ribbon_start[i].radius * canvas_scale_factor,
        y: ribbon_start[i].y + Math.sin(ribbon_start[i].angle) * ribbon_start[i].radius * canvas_scale_factor
      };

      ctx.bezierCurveTo(cp1.x, cp1.y, cp2.x, cp2.y, p.x, p.y);
    }
    ctx.closePath();

    let ribbon_end = history[strip_end_i];

    ctx.moveTo(ribbon_end[0].x, ribbon_end[0].y);
    for (let i = ribbon_end.length - 1; i >= 0; i--) {
      let next_i = (i + ribbon_end.length - 1) % ribbon_end.length;
      let prev_i = (i + 1) % ribbon_end.length;

      let cp1 = {
        x: ribbon_end[prev_i].x + Math.cos(ribbon_end[prev_i].angle) * ribbon_end[prev_i].radius * canvas_scale_factor,
        y: ribbon_end[prev_i].y + Math.sin(ribbon_end[prev_i].angle) * ribbon_end[prev_i].radius * canvas_scale_factor
      };
      let p = { x: ribbon_end[i].x, y: ribbon_end[i].y };
      let cp2 = {
        x: ribbon_end[i].x - Math.cos(ribbon_end[i].angle) * ribbon_end[i].radius * canvas_scale_factor,
        y: ribbon_end[i].y - Math.sin(ribbon_end[i].angle) * ribbon_end[i].radius * canvas_scale_factor
      };

      ctx.bezierCurveTo(cp1.x, cp1.y, cp2.x, cp2.y, p.x, p.y);
    }
    ctx.closePath();

    ctx.fill();
  }
  ctx.globalAlpha = 1;

  requestAnimationFrame(Tick);
}

requestAnimationFrame(Tick);
