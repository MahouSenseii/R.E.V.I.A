import { mapSnapshot, parameterDefinitions, validateEndpoint } from './state.mjs';

export const pluginIdentity = { pluginName: 'Revia Presence', pluginDeveloper: 'Revia Project' };

export class VtsClient
{
    constructor(endpoint, { socketFactory = url => new WebSocket(url), timeoutMs = 3000 } = {})
    {
        this.endpoint = validateEndpoint(endpoint);
        this.socketFactory = socketFactory;
        this.timeoutMs = timeoutMs;
        this.pending = new Map();
        this.nextId = 0;
        this.authenticated = false;
    }

    async connect()
    {
        this.socket = this.socketFactory(this.endpoint);
        this.socket.addEventListener('message', event => this.receive(event.data));
        this.socket.addEventListener('close', () => this.rejectPending('Renderer connection closed.'));
        this.socket.addEventListener('error', () => this.rejectPending('Renderer connection failed.'));
        await new Promise((resolve, reject) =>
        {
            const finish = error =>
            {
                clearTimeout(timer);
                this.socket.removeEventListener('open', opened);
                this.socket.removeEventListener('error', failed);
                this.socket.removeEventListener('close', failed);
                if (error) reject(error); else resolve();
            };
            const opened = () => finish();
            const failed = () => finish(new Error('Cannot connect to VTube Studio. Enable its Plugin API on the configured port.'));
            const timer = setTimeout(() =>
            {
                finish(new Error('Renderer connection timed out.'));
                this.close();
            }, this.timeoutMs);
            this.socket.addEventListener('open', opened);
            this.socket.addEventListener('error', failed);
            this.socket.addEventListener('close', failed);
        });
    }

    request(messageType, data = {}, timeoutMs = this.timeoutMs, allowCancelled = false)
    {
        if (this.cancelled && !allowCancelled) return Promise.reject(new Error('Renderer operation cancelled.'));
        if (this.socket?.readyState !== 1) return Promise.reject(new Error('Renderer connection closed.'));
        const requestID = `revia-${++this.nextId}`;
        return new Promise((resolve, reject) =>
        {
            const timer = setTimeout(() =>
            {
                this.pending.delete(requestID);
                reject(new Error(`${messageType} timed out.`));
            }, timeoutMs);
            this.pending.set(requestID, { resolve, reject, timer, expected: messageType.replace(/Request$/, 'Response') });
            try
            {
                this.socket.send(JSON.stringify({ apiName: 'VTubeStudioPublicAPI', apiVersion: '1.0',
                    requestID, messageType, data }));
            }
            catch
            {
                clearTimeout(timer);
                this.pending.delete(requestID);
                reject(new Error('Renderer send failed.'));
            }
        });
    }

    receive(body)
    {
        let message;
        try { message = JSON.parse(body); } catch { return; }
        if (message?.apiName !== 'VTubeStudioPublicAPI' || message.apiVersion !== '1.0') return;
        const call = this.pending.get(message.requestID);
        if (!call) return;
        this.pending.delete(message.requestID);
        clearTimeout(call.timer);
        if (message.messageType === 'APIError')
        {
            // Remote text is deliberately excluded: it can echo request credentials.
            call.reject(new Error(`VTube Studio API error ${Number.isInteger(message.data?.errorID) ? message.data.errorID : 'unknown'}`));
        }
        else if (message.messageType !== call.expected)
            call.reject(new Error('Unexpected renderer response type.'));
        else call.resolve(message.data ?? {});
    }

    async authenticate(token)
    {
        this.authenticated = false;
        const response = await this.request('AuthenticationRequest', { ...pluginIdentity, authenticationToken: token });
        if (response.authenticated !== true) throw new Error('Plugin authentication denied or revoked.');
        this.authenticated = true;
    }

    async initializeParameters()
    {
        this.requireAuthentication();
        for (const definition of parameterDefinitions) await this.request('ParameterCreationRequest', definition);
    }

    async inject(data)
    {
        this.requireAuthentication();
        return this.request('InjectParameterDataRequest', data);
    }

    requireAuthentication()
    {
        if (!this.authenticated) throw new Error('The renderer session must be authenticated.');
    }

    rejectPending(reason)
    {
        this.authenticated = false;
        this.cancelPending(reason);
    }

    cancelPending(reason)
    {
        for (const call of this.pending.values())
        {
            clearTimeout(call.timer);
            call.reject(new Error(reason));
        }
        this.pending.clear();
    }

    cancel()
    {
        this.cancelled = true;
        this.cancelPending('Renderer operation cancelled.');
    }

    close()
    {
        this.rejectPending('Renderer connection closed.');
        this.socket?.close();
    }

    async shutdown()
    {
        try
        {
            if (this.authenticated && this.socket?.readyState === 1)
                await this.request('InjectParameterDataRequest', mapSnapshot(null), Math.min(this.timeoutMs, 500), true);
        }
        finally { this.close(); }
    }
}
