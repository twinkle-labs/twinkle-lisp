;;    
;; Copyright (C) 2020, Twinkle Labs, LLC.
;;
;; This program is free software: you can redistribute it and/or modify
;; it under the terms of the GNU Affero General Public License as published
;; by the Free Software Foundation, either version 3 of the License, or
;; (at your option) any later version.
;;
;; This program is distributed in the hope that it will be useful,
;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;; GNU Affero General Public License for more details.
;;
;; You should have received a copy of the GNU Affero General Public License
;; along with this program.  If not, see <https://www.gnu.org/licenses/>.
;;

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;
;; peer.l -- Peer Connection and Message Exchange
;;
;; Provides implementation of peer and peerx processes.
;; A peer process can connect to a remote peerx process, and then
;; messages are exchanged on the duplex connection.
;;
;; The connection has 3 stages:
;;
;;  1. Handshake. Authentication.
;;  2. Protocol Negotiation. Choose the communication protocol,
;;     Peerx should check if the connecting peer has the access right
;;     to requested protocol and related resources.
;;  3. Protocol Exchange
;;     Exchanging messages based on a chosen protocol.
;; 
;; The connection is duplex. Peerx can also push messages to peer.
;; To avoid deadlock, either side should try to process incoming
;; messages first before sending messages.
;;
;; (keep-alive) messages are sent if the connection becomes idle.
;;
;; Timestamp should be exchanged when secure communication is established
;; so that we can know if there is replay attack.
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

(define peerx-timeout 60) ;; Exit if there is no activity during this period

;; disconnect if time is out of sync
(define (peer-time-out-of-sync? x y)
  (> (abs (- x y)) 60))
      
(define (start-peerx client-addr key-pair)
  ;; Start the peer process on server side
  ;; 
  
  (define self (this))

  (define (next-message)
    (define x (read in))
    (println "< " x)
    x)

  (define (post-message &rest x)
    (println "> " x)
    (write x out))
  
  (set-process-name "peerx")
  (define client-ip (car client-addr))
  (define in (open-socket-input))
  (define out (open-socket-output))
  (define handshake-ok false)
  (define protocol-manager false)
  (define client-uuid false)
  (define pk (cdr key-pair))
  (define vk (car key-pair))
  (define server-uuid (pubkey->address pk))
  (define (accept-handshake)
    (read-bytes in 256)
    (define y-hs (read-bytes in 256))
    (define y-pk (slice y-hs 0 65))
    (buffer-set! y-pk 0 #x04)

    (define shared-secret (ecdh vk y-pk))

    (define hs-msg (random-bytes 256))
    (buffer-set! hs-msg 1 (slice pk 1))
    (define salt (bitwise-xor (slice y-hs 128 144)
			      (slice hs-msg 128 144)))
    ;;(println "accept-handshake: shared secret=" shared-secret
    ;;	     " salt=" salt
    ;;	     )
    (define hdr (random-bytes 256))
    (buffer-set! hdr 0 (string->buffer "HTTP/1.1 200 OK\r\nConnection: Keep-Alive\r\n\r\n"))
    (write-buffer hdr out)
    (write-buffer hs-msg out)
    (flush out)
    
    (define key (sha256 shared-secret))
    (define iv (sha256 salt))

    (set-stream-cipher in 'aes-256-cfb8 key iv)
    (set-stream-cipher out 'aes-256-cfb8 key iv)
    (set! handshake-ok true)
    ;; (println "peerx handshake ok")
    )

  (define (check-protocol proto t)
    (define now (time))
    (if (peer-time-out-of-sync? t now)
	(error "Invalid time" t now)))
  
  (define (accept-protocol proto client-uuid client-pk shared-secret t)
    (check-protocol proto t)
    (if (not (eq? client-uuid (pubkey->address client-pk)))
	(error "Invalid client id"))
    (if (not (eq? shared-secret (sha256 (ecdh vk client-pk))))
	(error "Invalid client id"))
    (set! protocol-manager
	  (^{[in out args]
	     (load "proto/\{proto}-server.l")
	     (this)}
	    in out
	    (list :client-uuid client-uuid
		  :client-ip client-ip)))
    (post-message 'use proto  (time)))

  (defmethod (on-notify msg)
    (if protocol-manager
	(protocol-manager 'on-notify msg)))

  (define (dispatch-loop)
    (if (not (ready? in))
        (begin
          (if (and protocol-manager
		   (method? 'did-dispatch protocol-manager))
              (protocol-manager 'did-dispatch))
          (return)))
    
    (define x (next-message))
    (match x
           [*eof* (exit)]
           [(keep-alive) (dispatch-loop)]
           [(use proto client-id client-pk shared-secret t)
	    (accept-protocol proto client-id client-pk shared-secret t)]
           [else
	    (if protocol-manager
		(if (protocol-manager 'dispatch-message x)
                    (dispatch-loop))
		(error "Invalid message" x))]))

  (defmethod (run)
    (cond
     [handshake-ok
      (define x (catch (dispatch-loop)))
      (if (and (list? x) (eq? (car x) 'error))
	  (begin
	    (post-message 'bye x)
	    (exit)))
      (flush out)
      (verbose "flushed")
      (set-timeout peerx-timeout)]
     [(ready? in)
      (accept-handshake)]))

;; Always quit when timeout so we don't have zoombie
;; processes.
;; it's up to client to keep the connection alive
  (defmethod (timeout)
    (println "timeout -- bye")
    (exit))

;; Handshake and protocol negotiation must be done within
;; a limited time frame
  (set-timeout 10)
  (this) ;; End of start-peerx
  )

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

(define (start-peer server-uuid server-addr server-port client-kp protocol &rest args)
  ;;
  ;; Start a secure remote connection process.
  ;;
  ;; Server must return proof that it owns the uuid, and client must
  ;; prove its identity during protocol negotiation.
  ;;
  ;; Client can choose a protocol to talk to server
  ;; server needs to support the protocol as well.
  ;;
  ;; The handshake information returned from server must match server-uuid.
  ;; otherwise it could be a middle man hijacking the traffic.
  
  (set-process-name (concat protocol))

  (define (post-message &rest x)
    (println "> " x)
    (write x out))

  (define (next-message)
    (define x (read in))
    (println "< " x)
    x)

  (define server-ip server-addr)
  
  (if (not (ip-address? server-ip))
      (begin
	(println "Resolving " server-addr)
	(let [(ai (getaddrinfo server-addr))]
	  (if (null? ai)
	      (error "Can not resolve address" server-addr)
	      (set! server-ip (get-ipv4-from-addr-info ai))))))

  (println "Connecting to " server-uuid ":" server-ip ":" server-port)
  
  (if (not (connect server-ip server-port))
      (error "Can not connect to \{server-ip}:\{server-port}"))

  (println "Connected")

  (define out (open-socket-output))
  (define in (open-socket-input))
  (define handshake-ok false)
  (define server-pk)
  (define server-shared-secret) ;; used in protocol negotiation to establish the true identity of the requesting client.
  
  ;; use temporary key pair to setup communication.
  ;; identity authentication handled by protocol negotiation
  (define x-key-pair (keygen-secp256k1)) 
  
  (define x-hs (random-bytes 256))
  
  (define client-vk (car client-kp))
  (define client-pk (cdr client-kp))
  
  (define protocol-manager false)
  (define active false)
  
  (define (request-handshake)
    (define hdr (random-bytes 256))
    (buffer-set! hdr 0 (string->buffer "GET / HTTP/1.1\r\nHost: \{server-ip}:\{server-port}\r\nContent-Length: 0\r\n\r\n"))
    (write-buffer hdr out)
    
    (buffer-set! x-hs 1 (slice (cdr x-key-pair) 1))
   ; (println "offer-handshake:" x-hs)
    (write-buffer x-hs out)
    (flush out)
    (println "offer handshake")
    )

  (define (on-handshake)
    (read-bytes in 256)
    (define y-hs (read-bytes in 256))
    (println "handshake:" y-hs)
    (if (not (= (length y-hs) 256))
	(error "Bad handshake" y-hs))
    (define y-pk (slice y-hs 0 65))
    (buffer-set! y-pk 0 #x04)
    
    (set! server-pk y-pk)
    (if (not (null? client-kp))
	(set! server-shared-secret (sha256 (ecdh client-vk server-pk))))
    
    ;; Check that the returned public key from remote server
    ;; matches the UUID we want to speak to.
    ;; If the server returns a public key that matches this UUID
    ;; but he doesn't have the private key, then
    ;; the subsequent communication will be gibberish.
    (if (not (eq? (pubkey->address y-pk) server-uuid))
	(error "Server uuid mismatch:"(pubkey->address y-pk )))

    (define shared-secret (ecdh (car x-key-pair) y-pk))

    (define salt (bitwise-xor (slice y-hs 128 144) (slice x-hs 128 144)))
  ;;  (println "peerc: did-handshake: shared secret=" shared-secret
;;	     " salt=" salt
;;	     )

    (define key (sha256 shared-secret))
    (define iv (sha256 salt))

    (set-stream-cipher in 'aes-256-cfb8 key iv)
    (set-stream-cipher out 'aes-256-cfb8 key iv)
    (set! handshake-ok true)
    )

  (defmethod (on-request msg ack)
    (println "peer \{protocol} on request: " msg)
    (if protocol-manager
	(protocol-manager 'on-request msg ack)
	(ack (list 'error "Connecting"))))

  (define (check-protocol proto t)
    (if (not (eq? proto protocol))
	(error "Protocol mismatch" proto protocol))
    (define now (time))
    (if (peer-time-out-of-sync? t now)
	(error "Invalid time" t now))
    )

  (define (dispatch-loop)
    (if (not (ready? in))
        (begin
          (if (and protocol-manager
		   (method? 'did-dispatch protocol-manager))
              (protocol-manager 'did-dispatch))
          (return)))

    (define x (next-message))
    (match x
           [*eof* (exit)]
           [(keep-alive) (dispatch-loop)]
           [(use proto t)
            (if protocol-manager
                (error "Unexpected proto"))
            (check-protocol proto t)
            (set! protocol-manager (^{[in out args]
                                      (load "proto/\{proto}.l")
                                      (this)}
                                     in out args))
            (protocol-manager 'ready)
            (dispatch-loop)]
           [else
            (if protocol-manager
                (if (protocol-manager 'dispatch-message x)
                    (dispatch-loop))
                (error "Invalid message" x))]))
  
  (defmethod (run)
    (cond
     [handshake-ok
      (dispatch-loop)
      (flush out)
      (verbose "flushed")
      (set-timeout 30)]
     [(ready? in)
      (on-handshake)
      (post-message 'use protocol
		    (pubkey->address client-pk) client-pk
		    server-shared-secret (time))
      (flush out)]))

  (defmethod (timeout)
    (if (or (not handshake-ok) (not protocol-manager))
	(error "Handshake too long"))
    (post-message 'keep-alive)
    (flush out)
    )

  (request-handshake)
  (set-timeout 10)
  (this) ;; End of (start-peer)
  )


