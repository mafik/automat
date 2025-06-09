/*
Sample graph:

const graph = [
  {
    "id": 0,
    "name": "optimize_sfx",
    "steps_before": [],
    "steps_after": []
  },
  {
    "id": 1,
    "name": "build fasttrigo",
    "steps_before": [],
    "steps_after": [
      204,
      206,
      214,
      220,
      222
    ]
  },
  {
    "id": 2,
    "name": "get googletest",
    "steps_before": [],
    "steps_after": [
      3
    ]
  },
  {
    "id": 3,
    "name": "extract googletest",
    "steps_before": [
      2
    ],
    "steps_after": [
      4
    ]
  },
  ...
];
*/