FROM node:22-alpine
WORKDIR /app
COPY package.json server.js ./
COPY public ./public
RUN mkdir -p /data && chown node:node /data
ENV PORT=8080 DATA_FILE=/data/data.json
VOLUME /data
EXPOSE 8080
USER node
CMD ["node", "server.js"]
