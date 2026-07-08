function handler(event) {
    var request = event.request;
    if (request.uri === '/') {
        var headers = request.headers;
        var accept = headers.accept ? headers.accept.value : '';
        if (accept.indexOf('text/html') === -1) {
            request.uri = '/installer.sh';
        }
    }
    return request;
}
