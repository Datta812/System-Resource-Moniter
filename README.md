# System Resource Monitor

A Linux-based System Resource Monitoring application built using **C++, Python, Flask, and SQLite**. The project monitors system resource usage, stores historical data, and provides a web-based dashboard for visualization.

## Overview

The System Resource Monitor is designed to track important system performance metrics and maintain historical resource usage information.

It combines a C++ monitoring component with a Flask-based web dashboard and SQLite database for storing monitoring records.

## Features

- **CPU Monitoring:** Monitor CPU resource utilization.
- **Memory Monitoring:** Track memory usage and utilization.
- **Historical Data Storage:** Store resource monitoring records using SQLite.
- **Web Dashboard:** View system resource information through a Flask web application.
- **Data Visualization:** Display resource monitoring information through a dashboard.

## Technologies Used

| Technology | Purpose |
|---|---|
| C++ | System resource monitoring |
| Python | Backend application |
| Flask | Web dashboard |
| SQLite | Historical data storage |
| HTML | Dashboard structure |
| CSS | Dashboard styling |
| Linux | Operating environment |

## Project Structure

```text
System-Resource-Monitor/
│
├── project/
│   ├── dashboard/
│   │   ├── app.py
│   │   ├── static/
│   │   │   └── style.css
│   │   └── templates/
│   │       └── index.html
│   │
│   ├── data/
│   ├── docs/
│   │   └── screenshots/
│   │
│   ├── src/
│   │   └── main.cpp
│   │
│   ├── .gitignore
│   ├── README.md
│   ├── requirements.txt
│   └── run.sh
│
└── README.md
```

## Installation and Execution

### 1. Clone the Repository

```bash
git clone https://github.com/Datta812/System-Resource-Moniter.git
cd System-Resource-Moniter
```

### 2. Navigate to the Project

```bash
cd project
```

### 3. Install Python Dependencies

```bash
pip install -r requirements.txt
```

### 4. Run the Application

```bash
chmod +x run.sh
./run.sh
```

Follow the terminal instructions to access the dashboard.

## Database

SQLite is used to store historical system resource monitoring information.

## Author

**Datta812**

GitHub: [@Datta812](https://github.com/Datta812)

## License

This project is available for educational and learning purposes.
