package com.example.api;

import javax.ws.rs.GET;
import javax.ws.rs.Path;

@Path("/accounts")
public interface AccountEndpoint {

    @GET
    @Path("/profile")
    Response getProfile();

    @GET
    @Path("/health")
    @Unauthenticated
    Response getHealth();

    @GET
    @Path("/status")
    @Unauthenticated
    Response getStatus();
}
