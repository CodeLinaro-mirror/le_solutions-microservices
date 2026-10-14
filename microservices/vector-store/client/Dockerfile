# =============================================================================
# Standard Production Python Client Dockerfile
# Builds, tests, and sets up the standalone Python client for local execution.
# =============================================================================
FROM python:3.11-slim AS builder

# Prevent Python from writing .pyc files and enable unbuffered logging
ENV PYTHONDONTWRITEBYTECODE=1
ENV PYTHONUNBUFFERED=1

WORKDIR /app

# Install minimal package dependencies
RUN apt-get update && apt-get install -y --no-install-recommends                                   \
    build-essential                                                                             && \
    rm -rf /var/lib/apt/lists/*

# Copy dependency definition and install
COPY client/requirements.txt ./
RUN pip install --no-cache-dir -r requirements.txt

# Copy configuration and sources
COPY client/vector_store_client_config.json ./
COPY client/src/ ./src/
COPY client/tests/ ./tests/

# Set up PYTHONPATH so pytest and runner can resolve the src module
ENV PYTHONPATH=/app/src

# Run comprehensive unit tests as a mandatory build-time gate
RUN pytest tests/ -v

# Clean up tests from final container to minimize footprint
RUN rm -rf tests

# Run the python main application by default
CMD ["python", "src/main.py"]
