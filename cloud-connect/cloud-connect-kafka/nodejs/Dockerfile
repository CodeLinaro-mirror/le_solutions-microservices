FROM node:23-alpine

# Create app directory
WORKDIR /home/node/Cloud_Connect_Service

# Set a build argument
ARG BUILD_TYPE=release

# Set an environment variable based on the build argument
ENV BUILD_TYPE=${BUILD_TYPE}

# Create the directories required for configuration, certificates and it coverage
RUN mkdir /var/ssl
RUN mkdir /home/node/Cloud_Connect_Service/config
RUN mkdir /home/node/Cloud_Connect_Service/coverage
RUN chown -R node:node /var/ssl
RUN chown -R node:node /home/node/Cloud_Connect_Service/config
RUN chown -R node:node /home/node/Cloud_Connect_Service/coverage

# Install app dependencies
# A wildcard is used to ensure both package.json AND package-lock.json are copied
# where available (npm@5+)
COPY --chown=node:node package*.json ./

# If you are building your code for non-production environment
# RUN npm ci
# If you are building your code for production
RUN npm ci --only=production

# Bundle application sources
COPY --chown=node:node constants ./constants/
COPY --chown=node:node utils ./utils/
COPY --chown=node:node cloud-connect.js ./
COPY --chown=node:node .c8rc.json ./
COPY --chown=node:node index.js ./

# By default no command for coverage generation
ENV EXEC_CMD=""

# The command for coverage report generation will be dynamicaly generated
# and will be used in CMD[] section. To make it available there we have
# to make this environment variable part of profile.
RUN echo "export EXEC_CMD=\"npm start\"" >> /etc/profile.d/exec_cmd.sh;

# Overwrite EXEC_CMD conditionally and rewrite environmental variables file to apply changes
RUN if [ "$BUILD_TYPE" = "debug" ]; then \
        echo "export EXEC_CMD=\"npx c8 npm start\"" > /etc/profile.d/exec_cmd.sh; \
    fi
RUN chown node:node /etc/profile.d/exec_cmd.sh
RUN chmod +x /etc/profile.d/exec_cmd.sh

USER node

# Set the CMD to run your commands sequentially
CMD ["sh", "-c", "source /etc/profile.d/exec_cmd.sh; \
    echo $EXEC_CMD; \
    rm -rf /home/node/Cloud_Connect_Service/coverage/* && \
    $EXEC_CMD; \
    sleep 5"]