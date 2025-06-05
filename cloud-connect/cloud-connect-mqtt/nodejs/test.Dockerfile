FROM node:23-alpine

# Create app directory
WORKDIR /home/node/Cloud_Connect_Service

# Install app dependencies
# A wildcard is used to ensure both package.json AND package-lock.json are copied
# where available (npm@5+)
COPY --chown=node:node package*.json ./

# Install the MAKE and GCC compiler required by redis-memory-server node module along with dos2unix
RUN apk add --no-cache make build-base dos2unix

# Create the directories required for saving test execution reports and coverage
RUN mkdir /home/node/Cloud_Connect_Service/redis-binaries
RUN mkdir /home/node/Cloud_Connect_Service/reports
RUN mkdir /home/node/Cloud_Connect_Service/coverage

# If you are building your code for non-production environment
RUN npm ci
# If you are building your code for production
# RUN npm ci --only=production

RUN chown -R node:node /home/node/Cloud_Connect_Service
RUN chmod -R 777 /home/node/Cloud_Connect_Service

# Bundle application sources
COPY --chown=node:node __mocks__ ./__mocks__/
COPY --chown=node:node __test__ ./__test__/
COPY --chown=node:node constants ./constants/
COPY --chown=node:node utils ./utils/
COPY --chown=node:node .c8rc.json ./
COPY --chown=node:node babel.config.json ./
COPY --chown=node:node cloud-connect.js ./
COPY --chown=node:node index.js ./
COPY --chown=node:node jest.config.js ./

# Run dos2unix on container files
RUN find . -path ./node_modules -prune -o -type f -print0 | xargs -0 dos2unix

USER node

CMD ["sh", "-c", "rm -rf /home/node/Cloud_Connect_Service/coverage/* && \
    rm -rf /home/node/Cloud_Connect_Service/reports/*; \
    npm test"]