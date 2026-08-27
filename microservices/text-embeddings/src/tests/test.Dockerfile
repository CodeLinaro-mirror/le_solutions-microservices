FROM python:3.11-slim

WORKDIR /app

# Copy project files
COPY pyproject.toml setup.cfg ./
COPY web-api ./web-api

# Install dependencies
RUN pip install --no-cache-dir -e . && \
    pip install --no-cache-dir pytest pytest-cov

# Run tests
CMD ["pytest", "web-api/tests", "-v"]
