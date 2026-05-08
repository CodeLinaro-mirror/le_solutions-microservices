# ApproximateLocation


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of location approximation. Always &#x60;approximate&#x60;. | [default to 'approximate']
**country** | **str** | The two-letter ISO country code of the user, e.g. &#x60;US&#x60;. | [optional] 
**region** | **str** | Free text input for the region of the user, e.g. &#x60;California&#x60;. | [optional] 
**city** | **str** | Free text input for the city of the user, e.g. &#x60;San Francisco&#x60;. | [optional] 
**timezone** | **str** | The IANA timezone of the user, e.g. &#x60;America/Los_Angeles&#x60;. | [optional] 

## Example

```python
from openapi_client.models.approximate_location import ApproximateLocation

# TODO update the JSON string below
json = "{}"
# create an instance of ApproximateLocation from a JSON string
approximate_location_instance = ApproximateLocation.from_json(json)
# print the JSON string representation of the object
print(ApproximateLocation.to_json())

# convert the object into a dict
approximate_location_dict = approximate_location_instance.to_dict()
# create an instance of ApproximateLocation from a dict
approximate_location_from_dict = ApproximateLocation.from_dict(approximate_location_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


