package server

import (
	"context"
	"node-agent/util"
	"runtime/debug"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
)

type serverStream struct {
	grpc.ServerStream
	ctx context.Context
}

// Context returns the overridden context.
func (ss *serverStream) Context() context.Context {
	return ss.ctx
}

func requestHeaderValue(md metadata.MD, header string) string {
	var value string
	if md != nil {
		values := md[header]
		if len(values) > 0 {
			value = values[0]
		}
	}
	if value == "" {
		value = util.NewUUID().String()
	}
	return value
}

func withTracingIDs(srvCtx context.Context) context.Context {
	md, ok := metadata.FromIncomingContext(srvCtx)
	if !ok {
		md = nil
	}
	ctx := srvCtx
	for key, val := range util.TracingIDs {
		ctx = context.WithValue(
			ctx,
			val,
			requestHeaderValue(md, key),
		)
	}
	return ctx
}

// UnaryPanicHandler returns the ServerOption to handle panic occurred in the
// downstream unary handler.
func UnaryPanicHandler() grpc.UnaryServerInterceptor {
	return grpc.UnaryServerInterceptor(func(
		ctx context.Context,
		req interface{},
		info *grpc.UnaryServerInfo,
		handler grpc.UnaryHandler,
	) (response interface{}, err error) {
		defer func() {
			if e := recover(); e != nil {
				util.FileLogger().Errorf(ctx, "Panic occurred: %v", string(debug.Stack()))
				err = status.Errorf(codes.Internal, "Internal error occurred")
			}
		}()
		ctx = withTracingIDs(ctx)
		response, err = handler(ctx, req)
		return
	})
}

// StreamPanicHandler returns the interceptor to handle panic occurred in the
// downstream stream handler.
func StreamPanicHandler() grpc.StreamServerInterceptor {
	return grpc.StreamServerInterceptor(func(
		srv interface{},
		stream grpc.ServerStream,
		info *grpc.StreamServerInfo,
		handler grpc.StreamHandler,
	) (err error) {
		ctx := stream.Context()
		defer func() {
			if e := recover(); e != nil {
				util.FileLogger().Errorf(ctx, "Panic occurred: %v", string(debug.Stack()))
				err = status.Errorf(codes.Internal, "Internal error occurred")
			}
		}()
		ctx = withTracingIDs(ctx)
		stream = &serverStream{stream, ctx}
		err = handler(srv, stream)
		return
	})
}
